//! demo HTTP 路由 —— 列表 / 启停（demo-node 转发）
//!
//! 端点：
//!   GET  /api/demo/list                → {demos:[{name,path,kind}]}
//!   POST /api/demo/init  body {name}    发 demo_cmd start 到 demo-node
//!   POST /api/demo/stop                 发 demo_cmd stop 到 demo-node

use std::sync::Arc;

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