//! demo 服务 —— 列出 demo 目录，通过 dora demo_cmd 路由到 demo-node
//!
//! 架构：
//!   浏览器 → /api/demo/list                扫 demo/ 目录（找 init.sh 或 script.lua）
//!   浏览器 → /api/demo/upload_lua          multipart → 存 script.lua + demo.toml
//!   浏览器 → /api/demo/init body {name}     发 demo_cmd JSON 到 demo-node
//!   浏览器 → /api/demo/stop                  发 demo_cmd stop
//!
//! demo-node 负责实际启动 demo（subprocess init.sh 或 Lua script.lua）。
//! web-server 这里只做 HTTP API + 文件扫描 + dora 转发。

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::Arc;

use dora_node_api::DoraNode;
use serde::Serialize;
use tokio::sync::Mutex as TokioMutex;

#[derive(Debug, Clone, Serialize)]
pub struct DemoInfo {
    pub name: String,
    pub path: String,
    /// "binary" (有 init.sh) / "lua" (有 script.lua)
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

    /// 扫 demo 目录，列出含 init.sh 或 script.lua 的项。
    pub fn list(&self) -> Vec<DemoInfo> {
        let Ok(rd) = std::fs::read_dir(&self.base_dir) else {
            return vec![];
        };
        let mut out: Vec<DemoInfo> = rd
            .flatten()
            .filter(|e| e.path().is_dir())
            .filter_map(|e| {
                let dir = e.path();
                let has_init = dir.join("init.sh").is_file();
                let has_lua = dir.join("script.lua").is_file();
                let kind = if has_init {
                    "binary"
                } else if has_lua {
                    "lua"
                } else {
                    return None;
                };
                Some(DemoInfo {
                    name: e.file_name().to_string_lossy().to_string(),
                    path: dir.to_string_lossy().to_string(),
                    kind: kind.to_string(),
                })
            })
            .collect();
        out.sort_by(|a, b| a.name.cmp(&b.name));
        out
    }

    /// 上传 Lua 脚本（仅 Lua 类型）。同名 demo 不允许覆盖。
    pub fn upload_lua(
        &self,
        name: &str,
        code: &str,
        description: Option<&str>,
        icon: Option<&str>,
    ) -> Result<DemoInfo, String> {
        if name.is_empty()
            || !name
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
        {
            return Err(format!(
                "invalid demo name '{name}': only [A-Za-z0-9_-] allowed"
            ));
        }
        let dir = self.base_dir.join(name);
        if dir.exists() {
            return Err(format!("demo '{name}' already exists at {}", dir.display()));
        }
        std::fs::create_dir_all(&dir).map_err(|e| format!("mkdir: {e}"))?;
        std::fs::write(dir.join("script.lua"), code).map_err(|e| format!("write script.lua: {e}"))?;

        let toml = format!(
            r#"# 自动生成 by web-server /api/demo/upload_lua

[meta]
name = "{name}"
description = "{}"
icon = "{}"

[runtime]
engine = "lua"
"#,
            description.unwrap_or(""),
            icon.unwrap_or("📦"),
        );
        std::fs::write(dir.join("demo.toml"), toml).map_err(|e| format!("write demo.toml: {e}"))?;

        log::info!("[demo] uploaded '{name}' → {}", dir.display());
        Ok(DemoInfo {
            name: name.to_string(),
            path: dir.to_string_lossy().to_string(),
            kind: "lua".to_string(),
        })
    }

    pub async fn start(&self, name: &str) -> Result<(), String> {
        let payload = serde_json::json!({"command": "start", "name": name});
        self.send(&payload).await
    }

    pub async fn stop(&self) -> Result<(), String> {
        let payload = serde_json::json!({"command": "stop"});
        self.send(&payload).await
    }

    async fn send(&self, payload: &serde_json::Value) -> Result<(), String> {
        let bytes = serde_json::to_vec(payload).map_err(|e| format!("serialize: {e}"))?;
        let mut node = self.node.lock().await;
        node.send_output_bytes("demo_cmd".into(), BTreeMap::new(), bytes.len(), &bytes)
            .map_err(|e| format!("send demo_cmd: {e:?}"))?;
        Ok(())
    }
}