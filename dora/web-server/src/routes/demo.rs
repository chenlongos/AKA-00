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
    // 闸：preview 还开着就先把 camera 关掉，再启 demo。
    //
    // 为什么不直接同时跑：camera-node 持 /dev/videoX → tennis open 同一个
    // → uvcvideo 内核 USB 带宽锁互斥 → tennis 进 D-state（uninterruptible
    // sleep）→ 任何 SIGTERM/SIGKILL 都排不上调度 → 板子整体僵死。
    //
    // 之前修过的 demo-node 死锁是"信号发不出"，这条闸是从源头不让它进去。
    // sleep(500ms) 等 camera-node 真的 cam.close() + 释放 fd（发了 stop 命令
    // 不等于 camera-node 已执行），host 上够，板上 cvitek 流程若需更多调大。
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