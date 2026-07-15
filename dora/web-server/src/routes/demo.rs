//! demo HTTP 路由 —— 列表 / 启停（demo-node 转发）
//!
//! 端点：
//!   GET  /api/demo/list                → {demos:[{name,path,kind}]}
//!   POST /api/demo/init  body {name}    发 demo_cmd start 到 demo-node
//!   POST /api/demo/stop                 发 demo_cmd stop 到 demo-node

use std::sync::Arc;
use std::time::Duration;

use axum::{
    extract::State,
    Json, Router,
};
use axum::routing::{get, post};
use serde::Deserialize;

use crate::AppState;

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/demo/list", get(list))
        .route("/api/demo/init", post(init_demo))
        .route("/api/demo/stop", post(stop_demo))
}

async fn list(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    Json(serde_json::json!({ "demos": s.demo.list() }))
}

#[derive(Deserialize)]
struct InitBody {
    name: String,
}

async fn init_demo(
    State(s): State<Arc<AppState>>,
    Json(body): Json<InitBody>,
) -> (axum::http::StatusCode, Json<serde_json::Value>) {
    // ── 防御性清理 ──
    // 1. NPU 残留：上次的 stop 可能漏杀 NPU / CVitek TPU worker（demo-node 的
    //    200ms 升级 SIGKILL 用 kill(pid,0) 检查，只看 tennis 主进程，NPU
    //    worker 残留时漏发）。NPU device 一次只能被一个进程 open，残留的
    //    worker 会让新 demo 卡 D-state，表现为"无法停止"。
    //    pkill 扫 tennis / yolo_model / cviruntime 三个特征串，宁错杀。
    // 2. camera preview 还开着：camera-node 持 /dev/videoX → demo open 同一个
    //    → uvcvideo 内核 USB 带宽锁互斥 → demo 进 D-state（uninterruptible
    //    sleep）→ 任何 SIGTERM/SIGKILL 都排不上调度 → 板子整体僵死。
    log::info!("[demo-route] pre-start cleanup for demo '{}'", body.name);
    let _ = std::process::Command::new("pkill")
        .args(["-9", "-f", "tennis|yolo_model.cvimodel|cviruntime"])
        .output();
    // 给 kernel 时间回收 fd / TPU device
    tokio::time::sleep(Duration::from_millis(200)).await;

    if s.camera.is_active() {
        log::info!(
            "[demo-route] preview is on, closing camera before demo '{}' to avoid V4L2 deadlock",
            body.name
        );
        if let Err(e) = s.camera.close().await {
            return (
                axum::http::StatusCode::SERVICE_UNAVAILABLE,
                Json(serde_json::json!({
                    "error": format!("failed to close preview: {e}"),
                })),
            );
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }

    match s.demo.start(&body.name).await {
        Ok(()) => (
            axum::http::StatusCode::OK,
            Json(serde_json::json!({"status": "started", "name": body.name})),
        ),
        Err(e) => (
            axum::http::StatusCode::INTERNAL_SERVER_ERROR,
            Json(serde_json::json!({"error": e.to_string()})),
        ),
    }
}

async fn stop_demo(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    match s.demo.stop().await {
        Ok(()) => Json(serde_json::json!({"status": "stopped"})),
        Err(e) => Json(serde_json::json!({"error": e.to_string()})),
    }
}