//! dora demo-node —— 跑 demo 二进制（init.sh）的 dora 节点
//!
//! 对应 Python `app/routes/demo.py::demo_init` 的 subprocess 逻辑：
//!   - spawn demo/<name>/init.sh 作为子进程
//!   - process_group(0) 让 demo 独立进程组（优雅信号传播）
//!   - stop 时按 pgid 发 SIGTERM（让 worker 子进程有时间清理），等 2s，升级 SIGKILL
//!   - 子进程退出后清状态（reaper task 用 unique 持有 Child，无 mutex 共享）
//!
//! demo_cmd JSON 协议：
//!   {"command":"start","name":"tennis"}   启动 demo/tennis/init.sh
//!   {"command":"stop"}                     停止当前 demo（按 pgid）
//!   {"command":"status"}                   （调试）log 当前状态
//!
//! 状态通过 demo_status output 发给 web-server / state-node。
//!
//! 历史 bug:
//!   - reaper task 持锁 await child.wait()；stop task 也需同一锁调 start_kill。
//!     → 互相等死，stop 永远发不出 SIGKILL。
//!   - SIGKILL 单杀 PID 不带 pgid：tennis 若 fork NPU worker，worker 变孤儿。
//!   现在 Child 由 reaper 独占，RunningInfo 只存 name + pid，stop 用 libc+pgid。

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::process::Stdio;
use std::sync::{Arc, Mutex};

use arrow::array::AsArray;
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use serde::Deserialize;
use serde_json::json;
use tokio::process::Command;
use tokio::sync::Mutex as TokioMutex;

/// 当前在跑的 demo 运行时元数据。
///
/// ⚠ 不持有 `Child`。Child 由 reaper task 独占（unique 拥有），
/// 这样 stop 路径不需要 Child 上的 `&mut self`，可用 pid + libc
/// 直接发信号，避免 reaper / stop 互相等锁。
struct RunningInfo {
    name: String,
    pid: u32,
}

struct AppState {
    node: Arc<TokioMutex<DoraNode>>,
    running: Arc<Mutex<Option<RunningInfo>>>,
    demo_base: PathBuf,
}

/// 按 pgid（首选）或 pid 发送信号。pgid 优先能保证整个 demo 子进程树
/// 收到信号——tennis 可能 fork CVitek NPU worker，单杀 PID 会留孤儿。
unsafe fn kill_tree(pid: u32, sig: i32) {
    if pid == 0 {
        return;
    }
    let pid_t = pid as libc::pid_t;
    let pgid = libc::getpgid(pid_t);
    if pgid > 0 {
        // 负号 send to process group
        libc::kill(-pgid, sig);
    } else {
        libc::kill(pid_t, sig);
    }
}

impl AppState {
    /// 发布 demo 状态到 dora `demo_status` output。
    /// 错误只 log，不抛——controller 网络抖动不应让 demo-node 崩。
    async fn publish_status(&self, payload: serde_json::Value) {
        let bytes = serde_json::to_vec(&payload).unwrap_or_default();
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
            .process_group(0)  // 自己开 pgid，init.sh exec tennis 后继承
            .kill_on_drop(true)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        let mut child = match cmd.spawn() {
            Ok(c) => c,
            Err(e) => {
                log::warn!("[demo-node] spawn {}: {e}", init_script.display());
                return;
            }
        };
        // 等 cmd.spawn 后 pid 一定可读（API 保证）——不 .unwrap()，显式 fallback 0
        let pid = child.id().unwrap_or(0);
        log::info!("[demo-node] started '{name}' pid={pid}");

        // ⚠️ 先设 RunningInfo 再 spawn reaper，消除竞态：
        // 如果 child 在 spawn 和赋值之间快速退出，reaper 看到 running=None 就跳过，
        // 然后赋值 running=Some(...) 永远不被清除 → 后续 start 全被拒绝。
        *self.running.lock().unwrap() = Some(RunningInfo {
            name: name.clone(),
            pid,
        });

        // reaper：独占 Child，等它退出后清状态 + 发 idle status。
        // 用 `&mut` 直传不通过 Arc/Mutex，stop 路径压根不碰这个 Child。
        let running = self.running.clone();
        let node = self.node.clone();
        let name_for_reaper = name.clone();
        tokio::spawn(async move {
            let status = child.wait().await;
            log::info!("[demo-node] pid exited: name={name_for_reaper} status={status:?}");
            {
                let mut g = running.lock().unwrap();
                if let Some(r) = g.as_ref() {
                    if r.name == name_for_reaper {
                        *g = None;
                    }
                }
            }
            let payload = json!({
                "running": false,
                "name": serde_json::Value::Null,
                "pid": serde_json::Value::Null,
            });
            let bytes = serde_json::to_vec(&payload).unwrap_or_default();
            let mut n = node.lock().await;
            let _ = n.send_output_bytes(
                "demo_status".into(),
                BTreeMap::new(),
                bytes.len(),
                &bytes,
            );
        });

        // 发 running status（前端会收到并清掉 loading 状态）
        let payload = json!({
            "running": true,
            "name": name,
            "pid": pid,
        });
        self.publish_status(payload).await;
    }

    async fn stop(&self) {
        // take：原子地把 running 清出来。后续 stop 路径不持锁
        let prev = {
            let mut g = self.running.lock().unwrap();
            g.take()
        };
        let Some(r) = prev else {
            log::info!("[demo-node] stop called but no demo running");
            // 也发一条 idle status，把前端"还在跑"的状态同步回真实
            self.publish_status(json!({
                "running": false,
                "name": serde_json::Value::Null,
                "pid": serde_json::Value::Null,
            })).await;
            return;
        };
        log::info!("[demo-node] stopping '{}' pid={}", r.name, r.pid);

        // 用 libc 直接发信号——不需要 Child，绕开和 reaper 的锁竞争。
        // tokio Child::start_kill 在 stop 路径死锁是因为 reaper 持锁，
        // 现在根本不走 Child。
        //
        // SIGTERM 先发整个 pgid：让 worker 子进程（NPU / CVitek TPU）有机会清理。
        // 1.5s 后**检查整个 pgid**是否还活着（不是只看 pid！）——tennis 响应
        // SIGTERM 退出后 fork 出来的 NPU worker 可能残留，pgid 仍 alive；
        // 残留 NPU worker 会让下一次 start 卡 D-state，表现为"又无法停止"。
        // pgid 仍 alive 就发 SIGKILL 到整个 pgid。
        let pid = r.pid;
        unsafe { kill_tree(pid, libc::SIGTERM); }
        tokio::spawn(async move {
            tokio::time::sleep(std::time::Duration::from_millis(1500)).await;
            unsafe {
                let pid_t = pid as libc::pid_t;
                let pgid = libc::getpgid(pid_t);
                let still_alive = if pgid > 0 {
                    libc::kill(-pgid, 0) == 0
                } else {
                    // pgid 没了 (init.sh 早死 / 父进程没 setpgid) 的兜底
                    libc::kill(pid_t, 0) == 0
                };
                if still_alive {
                    log::warn!(
                        "[demo-node] pgid={} still alive after 1.5s SIGTERM, sending SIGKILL",
                        pgid
                    );
                    kill_tree(pid, libc::SIGKILL);
                }
            }
        });
    }

    fn status_log(&self) {
        let g = self.running.lock().unwrap();
        match g.as_ref() {
            Some(r) => log::info!(
                "[demo-node] status: running=true name={} pid={}",
                r.name, r.pid,
            ),
            None => log::info!("[demo-node] status: running=false"),
        }
    }
}

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
