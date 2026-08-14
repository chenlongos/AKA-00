//! 机械臂 HTTP 路由 —— 匹配前端 api.ts 中的 arm.* 调用
//!
//! 对应 `app/routes/arm.py` 的 3 个接口：
//!   GET  /api/arm/angles          → 返回当前 driver 与 angles（语义化格式）
//!   POST /api/arm/angles          → 写回 angles（body: {driver, angles}）
//!   POST /api/arm/angles/preview  → 写回并立即触发一次 set_angle（body: {driver, key, value, angles}）
//!
//! 角度配置结构（v0.5.2+ 语义化格式）:
//!   {
//!     "grab_position": {"servo0": 245, "servo1": 180},
//!     "lift_position": {"servo0": 200, "servo1": 180},
//!     "gripper_open": 150,
//!     "gripper_close": 90
//!   }

use std::sync::Arc;

use axum::{
    extract::State,
    Json, Router,
};
use axum::routing::{get, post};
use serde::Deserialize;

use crate::services::arm::{parse_arm_key, SemanticAngles};
use crate::AppState;

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/arm/angles", get(get_angles).post(save_angles))
        .route("/api/arm/angles/preview", post(preview))
        .route("/api/arm/angles/default", get(get_default).post(post_default))
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
    angles: SemanticAngles,
}

async fn save_angles(
    State(s): State<Arc<AppState>>,
    Json(body): Json<SaveBody>,
) -> (axum::http::StatusCode, Json<serde_json::Value>) {
    match s.arm.save_angles_semantic(&body.driver, &body.angles) {
        Ok(angles) => (
            axum::http::StatusCode::OK,
            Json(serde_json::json!({
                "status": "success",
                "driver": body.driver,
                "angles": angles,
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
    angles: SemanticAngles,
}

// ── GET/POST /api/arm/angles/default → arm_angles_default.json ──

fn default_path() -> std::path::PathBuf {
    let base = std::env::var("ARM_ANGLES_PATH")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|_| std::path::PathBuf::from("arm_angles.json"));
    base.with_file_name("arm_angles_default.json")
}

async fn get_default(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    let driver = s.arm.driver_name().to_string();
    let path = default_path();
    let angles: SemanticAngles = std::fs::read_to_string(&path)
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_else(|| s.arm.load_angles());
    Json(serde_json::json!({ "driver": driver, "angles": angles }))
}

#[derive(Deserialize)]
struct DefaultBody {
    driver: String,
    #[serde(default)]
    angles: SemanticAngles,
}

async fn post_default(
    State(s): State<Arc<AppState>>,
    Json(body): Json<DefaultBody>,
) -> (axum::http::StatusCode, Json<serde_json::Value>) {
    if body.driver != s.arm.driver_name() {
        return (
            axum::http::StatusCode::BAD_REQUEST,
            Json(serde_json::json!({ "error": "driver mismatch" })),
        );
    }
    let path = default_path();
    let json = serde_json::to_string_pretty(&body.angles).unwrap_or_default();
    match std::fs::write(&path, json) {
        Ok(()) => (
            axum::http::StatusCode::OK,
            Json(serde_json::json!({ "status": "success", "driver": body.driver, "angles": body.angles })),
        ),
        Err(e) => (
            axum::http::StatusCode::INTERNAL_SERVER_ERROR,
            Json(serde_json::json!({ "error": e.to_string() })),
        ),
    }
}

async fn preview(
    State(s): State<Arc<AppState>>,
    Json(body): Json<PreviewBody>,
) -> (axum::http::StatusCode, Json<serde_json::Value>) {
    if let Err(msg) = s.arm.save_angles_semantic_raw(&body.driver, &body.angles) {
        return (
            axum::http::StatusCode::BAD_REQUEST,
            Json(serde_json::json!({ "error": msg })),
        );
    }

    // 解析 key → servo_id
    // - "grab_position.servo0" → 0 (ZP10S) / 1 (STS3215)
    // - "gripper_open" / "gripper_close" → 2 (ZP10S) / 3 (STS3215)
    // - 旧格式 "servo0_prepare" → 0
    let servo_id = match parse_arm_key(&body.key) {
        Some(id) => id,
        None => {
            // gripper_open / gripper_close → 夹爪舵机 ID
            if body.key == "gripper_open" || body.key == "gripper_close" {
                s.arm.gripper_servo_id()
            } else {
                return (
                    axum::http::StatusCode::BAD_REQUEST,
                    Json(serde_json::json!({
                        "error": format!("invalid key: {}", body.key)
                    })),
                );
            }
        }
    };

    let _ = s.arm.set_angle(servo_id, body.value).await;
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
