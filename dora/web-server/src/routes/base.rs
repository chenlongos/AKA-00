//! 底盘 HTTP 路由 —— 对应 Python `app/routes/base.py`
//!
//!   POST /api/base/reinitialize    → 重置电机底盘（tt_pid ESP32）

use std::sync::Arc;

use axum::{
    extract::State,
    Json, Router,
};
use axum::routing::post;

use crate::AppState;

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/base/reinitialize", post(reinitialize))
}

async fn reinitialize(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    s.motor.reinitialize().await;
    Json(serde_json::json!({"status": "success", "reinitialize": true}))
}
