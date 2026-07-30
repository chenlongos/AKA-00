//! 系统路由 —— IP 查询 + heartbeat（对应 Python `app/routes/system.py`）

use std::sync::Arc;

use axum::{
    extract::State,
    Json, Router,
};
use axum::routing::get;

use crate::AppState;

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/system/ip", get(get_ip))
        .route("/api/system/info", get(get_info))
        .route("/api/system/heartbeat", get(heartbeat))
}

async fn get_info(State(_s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    let ip = crate::routes::control::detect_local_ip().await;
    Json(serde_json::json!({
        "ip": ip,
        "mac": get_device_mac(),
    }))
}

async fn get_ip(State(_s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    // 复用 control.rs 的 detect_local_ip 逻辑：UDP 探测优先，wlan1/wlan0 兜底
    let ip = crate::routes::control::detect_local_ip().await;
    Json(serde_json::json!({ "ip": ip }))
}

/// 取设备物理 MAC 地址。
///
/// 设备的物理地址 = 板载 wifi 芯片的 MAC = wlan0 的 MAC（SoC eFuse 烧录，
/// init_ap_web.sh 用 wlan0 MAC 后缀生成 SSID 证实了这一点）。
///
/// 之前扫描所有接口是过度设计 —— 容易扫到虚拟接口 / 随机 MAC，反而
/// 不准。Python 也只读 wlan0。对齐 Python 行为：直接读 wlan0。
///
/// wlan0 不存在时（init_ap_web.sh 没跑、wifi 驱动没加载）返回 "unknown"，
/// 不假装 / 不掩盖问题。
fn get_device_mac() -> String {
    std::fs::read_to_string("/sys/class/net/wlan0/address")
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "unknown".to_string())
}

/// 心跳：前端轮询用，红灯依赖这个判断 robot 是否在线
/// 对应 Python `app/routes/system.py::heartbeat`：
///   mac_address = get_mac_address("wlan0")
///   return {"status":"ok","service":"AKA-00","mac_address":...}
async fn heartbeat(State(_s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    Json(serde_json::json!({
        "status": "ok",
        "service": "AKA-00",
        "mac_address": get_device_mac(),
    }))
}