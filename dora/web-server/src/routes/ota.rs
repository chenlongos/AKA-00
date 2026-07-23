//! OTA 固件升级路由 — 对应 Python app/routes/ota.py

use std::path::PathBuf;
use std::sync::Arc;

use axum::{extract::State, Json, Router};
use axum::routing::get;
use serde::Serialize;

use crate::AppState;

fn version_file() -> PathBuf {
    std::env::var("DORA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/root/AKA-00"))
        .join("VERSION")
}

fn read_version() -> (String, String) {
    let path = version_file();
    if let Ok(s) = std::fs::read_to_string(&path) {
        let parts: Vec<&str> = s.trim().split_whitespace().collect();
        match parts.len() {
            2 => (parts[0].to_string(), parts[1].to_string()),
            1 => (parts[0].to_string(), parts[0].to_string()),
            _ => ("unknown".into(), "0".into()),
        }
    } else {
        ("unknown".into(), "0".into())
    }
}

#[derive(Serialize)]
struct VersionResponse {
    version: String,
    updated: i64,
    service: String,
}

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/ota/version", get(get_version))
        .route("/api/ota/check", get(check_update))
}

async fn get_version(State(_s): State<Arc<AppState>>) -> Json<VersionResponse> {
    let (ver, ts) = read_version();
    Json(VersionResponse {
        version: ver,
        updated: ts.parse().unwrap_or(0),
        service: "AKA-00".into(),
    })
}

async fn check_update(State(_s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    let (cur_ver, cur_ts) = read_version();
    let cur_ts: i64 = cur_ts.parse().unwrap_or(0);

    // 读取 OTA check URL（从环境变量或 config.toml）
    let check_url = std::env::var("OTA_CHECK_URL").unwrap_or_default();

    let (remote_ts, latest_ver, hw_desc, sw_desc, url) = if check_url.is_empty() {
        (0, String::new(), String::new(), String::new(), String::new())
    } else {
        fetch_remote(&check_url).await.unwrap_or_default()
    };

    let has_update = remote_ts > 0 && remote_ts > cur_ts;

    Json(serde_json::json!({
        "current_version": cur_ver,
        "current_updated": cur_ts,
        "remote_updated": remote_ts,
        "update_available": has_update,
        "latest_version": latest_ver,
        "hardware_desc": hw_desc,
        "software_desc": sw_desc,
        "url": url,
    }))
}

async fn fetch_remote(url: &str) -> Result<(i64, String, String, String, String), String> {
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(5))
        .build()
        .map_err(|e| e.to_string())?;

    let resp = client.get(url).send().await.map_err(|e| e.to_string())?;
    let data: serde_json::Value = resp.json().await.map_err(|e| e.to_string())?;

    let inner = data.get("data").unwrap_or(&data);
    let url = inner.get("imageUrl").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let latest = inner.get("versionNumber").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let hw = inner.get("hardwareDesc").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let sw = inner.get("softwareDesc").and_then(|v| v.as_str()).unwrap_or("").to_string();

    // 解析 updatedAt → Unix timestamp
    let ts = inner.get("updatedAt")
        .and_then(|v| v.as_str())
        .map(|s| {
            // "2026-07-21T10:15:39.422Z" → seconds since epoch
            let s = s.replace('Z', "").split('.').next().unwrap_or("").to_string();
            chrono::NaiveDateTime::parse_from_str(&s, "%Y-%m-%dT%H:%M:%S")
                .map(|dt| dt.and_utc().timestamp())
                .unwrap_or(0)
        })
        .unwrap_or(0);

    Ok((ts, latest, hw, sw, url))
}
