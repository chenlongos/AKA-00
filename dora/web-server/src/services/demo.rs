//! demo 服务 —— 列出 demo 目录，通过 dora demo_cmd 路由到 demo-node
//!
//! 架构：
//!   浏览器 → /api/demo/list                扫 demo/ 目录（找含 init.sh 的子目录）
//!   浏览器 → /api/demo/init body {name}     发 demo_cmd JSON 到 demo-node
//!   浏览器 → /api/demo/stop                  发 demo_cmd stop
//!
//! demo-node 负责实际启动 demo（subprocess init.sh）。
//! web-server 这里只做 HTTP API + 文件扫描 + dora 转发。

use std::path::PathBuf;
use std::sync::Arc;

use dora_node_api::DoraNode;
use eyre;
use serde::Serialize;
use tokio::sync::Mutex as TokioMutex;

use super::dora_send;

#[derive(Debug, Clone, Serialize)]
pub struct DemoInfo {
    pub name: String,
    pub path: String,
    /// "binary" (有 init.sh)
    pub kind: String,
}

pub struct DemoService {
    node: Arc<TokioMutex<DoraNode>>,
    base_dir: PathBuf,
}

impl DemoService {
    pub fn new(node: Arc<TokioMutex<DoraNode>>) -> Self {
        let base_dir = std::env::var("DEMO_BASE_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|_| PathBuf::from("demo"));
        log::info!("[demo] base dir = {}", base_dir.display());
        Self { node, base_dir }
    }

    /// 扫 demo 目录，列出含 init.sh 的项。
    pub fn list(&self) -> Vec<DemoInfo> {
        let Ok(rd) = std::fs::read_dir(&self.base_dir) else {
            return vec![];
        };
        let mut out: Vec<DemoInfo> = rd
            .flatten()
            .filter(|e| e.path().is_dir())
            .filter(|e| e.path().join("init.sh").is_file())
            .map(|e| {
                let dir = e.path();
                DemoInfo {
                    name: e.file_name().to_string_lossy().to_string(),
                    path: dir.to_string_lossy().to_string(),
                    kind: "binary".to_string(),
                }
            })
            .collect();
        out.sort_by(|a, b| a.name.cmp(&b.name));
        out
    }

    pub async fn start(&self, name: &str) -> eyre::Result<()> {
        let payload = serde_json::json!({"command": "start", "name": name});
        self.send(&payload).await
    }

    pub async fn stop(&self) -> eyre::Result<()> {
        let payload = serde_json::json!({"command": "stop"});
        self.send(&payload).await
    }

    async fn send(&self, payload: &serde_json::Value) -> eyre::Result<()> {
        let bytes = serde_json::to_vec(payload).map_err(|e| eyre::eyre!("serialize: {e}"))?;
        dora_send::send_output(&self.node, "demo_cmd", &bytes).await
    }
}