//! 电机 HTTP 路由 —— 匹配前端 api.ts 中的 motor.* 调用
//!
//! 对应 `app/routes/motor.py`。

use std::sync::Arc;

use axum::{
    extract::{Query, State},
    Json, Router,
};
use axum::routing::get;
use serde::Deserialize;

use crate::AppState;

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/motor/status", get(status))
        .route("/api/motor/direct", get(direct))
}

#[derive(Deserialize, Default)]
struct DirectParams {
    left: Option<i32>,
    right: Option<i32>,
    duration: Option<u32>,
}

async fn direct(
    State(s): State<Arc<AppState>>,
    Query(p): Query<DirectParams>,
) -> Json<serde_json::Value> {
    s.motor.direct(p.left.unwrap_or(0), p.right.unwrap_or(0), p.duration.unwrap_or(0)).await;
    Json(serde_json::json!({"ok": true}))
}

async fn status(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    let s = s.motor.status();
    Json(serde_json::json!({
        "left_speed":  s.left_speed,    // m/s
        "right_speed": s.right_speed,   // m/s
    }))
}
