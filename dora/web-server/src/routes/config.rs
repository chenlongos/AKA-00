//! 行驶速度配置路由 — 读写 speed_config.json

use std::sync::Arc;
use std::path::PathBuf;

use axum::{extract::State, Json, Router};
use axum::routing::{get, post};
use serde::{Deserialize, Serialize};

use crate::AppState;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SpeedConfig {
    pub forward_speed: i32,
    pub turn_speed: i32,
}

impl Default for SpeedConfig {
    fn default() -> Self {
        Self { forward_speed: 50, turn_speed: 50 }
    }
}

impl SpeedConfig {
    fn config_path() -> PathBuf {
        std::env::var("DORA_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|_| PathBuf::from("/root/AKA-00"))
            .join("speed_config.json")
    }

    pub fn load() -> Self {
        let path = Self::config_path();
        std::fs::read_to_string(&path)
            .ok()
            .and_then(|s| serde_json::from_str(&s).ok())
            .unwrap_or_default()
    }

    fn save(&self) -> Result<(), String> {
        let path = Self::config_path();
        let json = serde_json::to_string(self).map_err(|e| e.to_string())?;
        std::fs::write(&path, json).map_err(|e| e.to_string())
    }
}

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/config/speed", get(get_speed).post(post_speed))
}

async fn get_speed(State(_s): State<Arc<AppState>>) -> Json<SpeedConfig> {
    Json(SpeedConfig::load())
}

async fn post_speed(
    State(_s): State<Arc<AppState>>,
    Json(body): Json<SpeedConfig>,
) -> Json<serde_json::Value> {
    match body.save() {
        Ok(()) => Json(serde_json::json!({"status": "ok"})),
        Err(e) => Json(serde_json::json!({"error": e})),
    }
}
