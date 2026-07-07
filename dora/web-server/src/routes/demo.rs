//! demo HTTP 路由 —— 列表 / 上传 Lua / 启停（demo-node 转发）
//!
//! 端点：
//!   GET  /api/demo/list                → {demos:[{name,path,kind}]}
//!   POST /api/demo/upload_lua          multipart: name, code, description?, icon?
//!   POST /api/demo/init  body {name}    发 demo_cmd start 到 demo-node
//!   POST /api/demo/stop                 发 demo_cmd stop 到 demo-node

use std::sync::Arc;

use axum::{
    extract::{Multipart, State},
    Json, Router,
};
use axum::routing::{get, post};
use serde::Deserialize;

use crate::AppState;

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/demo/list", get(list))
        .route("/api/demo/upload_lua", post(upload_lua))
        .route("/api/demo/init", post(init_demo))
        .route("/api/demo/stop", post(stop_demo))
}

async fn list(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    Json(serde_json::json!({ "demos": s.demo.list() }))
}

async fn upload_lua(
    State(s): State<Arc<AppState>>,
    mut multipart: Multipart,
) -> (axum::http::StatusCode, Json<serde_json::Value>) {
    let mut name: Option<String> = None;
    let mut code: Option<String> = None;
    let mut description: Option<String> = None;
    let mut icon: Option<String> = None;

    while let Ok(Some(field)) = multipart.next_field().await {
        let n = field.name().unwrap_or("").to_string();
        match n.as_str() {
            "name" => name = field.text().await.ok(),
            "code" => code = field.text().await.ok(),
            "description" => description = field.text().await.ok(),
            "icon" => icon = field.text().await.ok(),
            _ => {}
        }
    }

    let (Some(name), Some(code)) = (name, code) else {
        return (
            axum::http::StatusCode::BAD_REQUEST,
            Json(serde_json::json!({"error": "name and code are required"})),
        );
    };

    match s.demo.upload_lua(&name, &code, description.as_deref(), icon.as_deref()) {
        Ok(info) => (
            axum::http::StatusCode::OK,
            Json(serde_json::json!({"status": "uploaded", "info": info})),
        ),
        Err(e) => (
            axum::http::StatusCode::BAD_REQUEST,
            Json(serde_json::json!({"error": e})),
        ),
    }
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
            Json(serde_json::json!({"error": e})),
        ),
    }
}

async fn stop_demo(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    match s.demo.stop().await {
        Ok(()) => Json(serde_json::json!({"status": "stopped"})),
        Err(e) => Json(serde_json::json!({"error": e})),
    }
}