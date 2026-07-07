//! dora demo-node —— 跑 demo 二进制（init.sh）的 dora 节点
//!
//! 对应 Python `app/routes/demo.py::demo_init` 的 subprocess 逻辑：
//!   - spawn demo/<name>/init.sh 作为子进程
//!   - start_new_session=true 等价：process_group(0) 让 demo 独立进程组
//!   - stop 时按 PID 发 SIGKILL，等 3s
//!   - 子进程退出后清状态（reaper task）
//!
//! demo_cmd JSON 协议：
//!   {"command":"start","name":"tennis"}   启动 demo/tennis/init.sh
//!   {"command":"stop"}                     停止当前 demo
//!   {"command":"status"}                   （调试）log 当前状态
//!
//! 状态通过 demo_status output 发给 web-server / state-node。

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::process::Stdio;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use arrow::array::AsArray;
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use serde::Deserialize;
use serde_json::json;
use tokio::process::{Child, Command};
use tokio::sync::Mutex as TokioMutex;

#[derive(Debug, Deserialize)]
#[serde(tag = "command")]
enum DemoCmd {
    #[serde(rename = "start")]
    Start { name: String },
    #[serde(rename = "stop")]
    Stop,
    #[serde(rename = "status")]
    Status,
}

struct RunningDemo {
    name: String,
    child: Arc<TokioMutex<Child>>,
}

struct AppState {
    node: Arc<TokioMutex<DoraNode>>,
    running: Arc<Mutex<Option<RunningDemo>>>,
    demo_base: PathBuf,
}

impl AppState {
    #[allow(dead_code)]
    async fn publish_status(&self) {
        let snap = {
            let g = self.running.lock().unwrap();
            match g.as_ref() {
                Some(r) => {
                    let pid = r.child.try_lock().ok().and_then(|mut g| g.id());
                    json!({"running": true, "name": r.name, "pid": pid})
                }
                None => json!({"running": false, "name": serde_json::Value::Null, "pid": serde_json::Value::Null}),
            }
        };
        let bytes = serde_json::to_vec(&snap).unwrap_or_default();
        let mut n = self.node.lock().await;
        let _ = n.send_output_bytes(
            "demo_status".into(),
            BTreeMap::new(),
            bytes.len(),
            &bytes,
        );
    }

    async fn start(&self, name: String) {
        let demo_dir = self.demo_base.join(&name);
        let init_script = demo_dir.join("init.sh");
        if !demo_dir.is_dir() {
            log::warn!("[demo-node] demo dir not found: {}", demo_dir.display());
            return;
        }
        if !init_script.is_file() {
            log::warn!("[demo-node] init.sh not found: {}", init_script.display());
            return;
        }

        // 已经有 demo 在跑 → 拒绝
        {
            let g = self.running.lock().unwrap();
            if g.is_some() {
                log::warn!("[demo-node] demo already running, ignoring start '{name}'");
                return;
            }
        }

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&init_script, std::fs::Permissions::from_mode(0o755));
        }

        let mut cmd = Command::new(&init_script);
        cmd.current_dir(&demo_dir)
            .process_group(0)
            .kill_on_drop(true)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        let child = match cmd.spawn() {
            Ok(c) => c,
            Err(e) => {
                log::warn!("[demo-node] spawn {}: {e}", init_script.display());
                return;
            }
        };
        let pid = child.id();
        log::info!("[demo-node] started '{name}' pid={pid:?}");

        let child_arc = Arc::new(TokioMutex::new(child));

        // reaper：等子进程退出清状态
        let running_for_reaper = self.running.clone();
        let node_for_reaper = self.node.clone();
        let name_for_reaper = name.clone();
        let child_for_reaper = child_arc.clone();
        tokio::spawn(async move {
            let status = {
                let mut g = child_for_reaper.lock().await;
                g.wait().await
            };
            log::info!("[demo-node] pid exited: name={name_for_reaper} status={status:?}");
            {
                let mut g = running_for_reaper.lock().unwrap();
                if let Some(r) = g.as_ref() {
                    if r.name == name_for_reaper {
                        *g = None;
                    }
                }
            }
            // 发布 idle status
            let bytes = serde_json::to_vec(&json!({
                "running": false,
                "name": serde_json::Value::Null,
                "pid": serde_json::Value::Null,
            }))
            .unwrap_or_default();
            let mut n = node_for_reaper.lock().await;
            let _ = n.send_output_bytes(
                "demo_status".into(),
                BTreeMap::new(),
                bytes.len(),
                &bytes,
            );
        });

        *self.running.lock().unwrap() = Some(RunningDemo {
            name: name.clone(),
            child: child_arc,
        });

        // 发 running status
        let bytes = serde_json::to_vec(&json!({
            "running": true,
            "name": name,
            "pid": pid,
        }))
        .unwrap_or_default();
        let mut n = self.node.lock().await;
        let _ = n.send_output_bytes(
            "demo_status".into(),
            BTreeMap::new(),
            bytes.len(),
            &bytes,
        );
    }

    async fn stop(&self) {
        let prev = {
            let mut g = self.running.lock().unwrap();
            g.take()
        };
        let Some(r) = prev else {
            log::info!("[demo-node] stop called but no demo running");
            // 也发一条 idle status
            let bytes = serde_json::to_vec(&json!({
                "running": false,
                "name": serde_json::Value::Null,
                "pid": serde_json::Value::Null,
            }))
            .unwrap_or_default();
            let mut n = self.node.lock().await;
            let _ = n.send_output_bytes(
                "demo_status".into(),
                BTreeMap::new(),
                bytes.len(),
                &bytes,
            );
            return;
        };
        let pid = r.child.try_lock().ok().and_then(|mut g| g.id());
        log::info!("[demo-node] stopping '{}' pid={pid:?}", r.name);

        // 发 SIGKILL + 等 3s（想 SIGTERM 先 → 等 → SIGKILL 用 libc，先简化）
        let child = r.child.clone();
        tokio::spawn(async move {
            let mut g = child.lock().await;
            let _ = g.start_kill();
            let _ = tokio::time::timeout(Duration::from_secs(3), g.wait()).await;
        });
    }

    fn status_log(&self) {
        let g = self.running.lock().unwrap();
        match g.as_ref() {
            Some(r) => log::info!(
                "[demo-node] status: running=true name={} pid={:?}",
                r.name,
                r.child.try_lock().ok().and_then(|g| g.id()),
            ),
            None => log::info!("[demo-node] status: running=false"),
        }
    }
}

fn main() -> Result<()> {
    let config_for_log = dora_config::Config::load();
    env_logger::Builder::from_env(
        env_logger::Env::default().default_filter_or(&config_for_log.logging.level),
    )
    .format_timestamp_millis()
    .init();

    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(run()).unwrap_or_else(|e| log::error!("demo-node error: {e:?}"));
    Ok(())
}

async fn run() -> Result<()> {
    let demo_base = PathBuf::from(
        std::env::var("DEMO_BASE_DIR").unwrap_or_else(|_| "demo".into()),
    );
    log::info!("[demo-node] demo base dir: {}", demo_base.display());

    let (node, mut events) = DoraNode::init_from_env().wrap_err("Failed to init dora node")?;
    log::info!("[demo-node] Dora node initialized");

    let app = Arc::new(AppState {
        node: Arc::new(TokioMutex::new(node)),
        running: Arc::new(Mutex::new(None)),
        demo_base,
    });

    while let Some(event) = events.next().await {
        match event {
            Event::Input { id, data, .. } => {
                if id.to_string() != "demo_cmd" {
                    continue;
                }
                let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
                let bytes = uint8_arr.values();

                let cmd: DemoCmd = match serde_json::from_slice(bytes) {
                    Ok(c) => c,
                    Err(e) => {
                        log::warn!("[demo-node] parse demo_cmd: {e:?}");
                        continue;
                    }
                };

                match cmd {
                    DemoCmd::Start { name } => app.start(name).await,
                    DemoCmd::Stop => app.stop().await,
                    DemoCmd::Status => app.status_log(),
                }
            }
            Event::Stop(cause) => {
                log::info!("[demo-node] Stop: {cause:?}");
                app.stop().await;
                break;
            }
            _ => {}
        }
    }

    log::info!("[demo-node] Shutdown");
    Ok(())
}