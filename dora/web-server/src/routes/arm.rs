//! 机械臂 HTTP 路由 —— 匹配前端 api.ts 中的 arm.* 调用
//!
//! 对应 `app/routes/arm.py` 的 3 个接口：
//!   GET  /api/arm/angles          → 返回当前 driver 与 angles
//!   POST /api/arm/angles          → 写回 angles（body: {driver, angles}）
//!   POST /api/arm/angles/preview  → 写回并立即触发一次 set_angle（body: {driver, key, value, angles}）

use std::collections::BTreeMap;
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
        .route("/api/arm/angles", get(get_angles).post(save_angles))
        .route("/api/arm/angles/preview", post(preview))
}

async fn get_angles(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    let driver = s.arm.driver_name().to_string();
    let angles = s.arm.load_angles();
    Json(serde_json::json!({
        "driver": driver,
        "angles": angles,
    }))
}

#[derive(Deserialize)]
struct SaveBody {
    driver: String,
    #[serde(default)]
    angles: BTreeMap<String, u16>,
}

async fn save_angles(
    State(s): State<Arc<AppState>>,
    Json(body): Json<SaveBody>,
) -> (axum::http::StatusCode, Json<serde_json::Value>) {
    match s.arm.save_angles(&body.driver, &body.angles) {
        Ok(()) => (
            axum::http::StatusCode::OK,
            Json(serde_json::json!({
                "status": "success",
                "driver": body.driver,
                "angles": s.arm.load_angles(),
            })),
        ),
        Err(msg) => (
            axum::http::StatusCode::BAD_REQUEST,
            Json(serde_json::json!({ "error": msg })),
        ),
    }
}

#[derive(Deserialize)]
struct PreviewBody {
    driver: String,
    key: String,
    value: u16,
    #[serde(default)]
    angles: BTreeMap<String, u16>,
}

/// "servo0_prepare" → 0；前端 key 形如 "servo<N>_*"
fn arm_key_to_servo_id(key: &str) -> Option<u8> {
    key.strip_prefix("servo")?.chars().next()?.to_digit(10).map(|n| n as u8)
}

async fn preview(
    State(s): State<Arc<AppState>>,
    Json(body): Json<PreviewBody>,
) -> (axum::http::StatusCode, Json<serde_json::Value>) {
    if let Err(msg) = s.arm.save_angles(&body.driver, &body.angles) {
        return (
            axum::http::StatusCode::BAD_REQUEST,
            Json(serde_json::json!({ "error": msg })),
        );
    }

    let servo_id = match arm_key_to_servo_id(&body.key) {
        Some(id) => id,
        None => {
            return (
                axum::http::StatusCode::BAD_REQUEST,
                Json(serde_json::json!({
                    "error": format!("invalid key: {}", body.key)
                })),
            );
        }
    };

    s.arm.set_angle(servo_id, body.value).await;
    (
        axum::http::StatusCode::OK,
        Json(serde_json::json!({
            "status": "success",
            "driver": body.driver,
            "key": body.key,
            "value": body.value,
            "servo_id": servo_id,
            "angles": s.arm.load_angles(),
        })),
    )
}