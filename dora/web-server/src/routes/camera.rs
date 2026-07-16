//! Camera HTTP 路由 —— 薄层，只做参数解析和序列化，业务逻辑委托给 CameraService
//!
//! 对应 Python 项目 `app/routes/camera.py` 的职责。
//! API 契约完全匹配 frontend/src/api.ts，不修改前端代码。

use std::sync::Arc;

use axum::{
    body::Body,
    extract::State,
    http::{header, Response, StatusCode},
    routing::{get, post},
    Json, Router,
};

use crate::AppState;

/// 注册所有 camera 路由（仿 Flask blueprint）
pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/camera/status", get(status))
        .route("/api/camera/open", post(open))
        .route("/api/camera/close", post(close))
        .route("/api/camera/stream", get(stream))
        .route("/api/camera/snapshot", get(snapshot))
}

const BOUNDARY: &str = "dora-frame";

// ── 响应类型 ──

#[derive(serde::Serialize)]
pub struct CameraStatus {
    pub camera_on: bool,
}

// ── GET /api/camera/status ──

pub async fn status(State(s): State<Arc<AppState>>) -> Json<CameraStatus> {
    Json(CameraStatus {
        camera_on: s.camera.is_active(),
    })
}

// ── POST /api/camera/open ──

pub async fn open(State(s): State<Arc<AppState>>) -> Result<Json<CameraStatus>, StatusCode> {
    s.camera.open().await.map_err(|e| {
        log::error!("[camera-route] open failed: {:?}", e);
        StatusCode::INTERNAL_SERVER_ERROR
    })?;
    Ok(Json(CameraStatus { camera_on: true }))
}

// ── POST /api/camera/close ──

pub async fn close(
    State(s): State<Arc<AppState>>,
) -> Result<Json<CameraStatus>, StatusCode> {
    s.camera.close().await.map_err(|e| {
        log::error!("[camera-route] close failed: {:?}", e);
        StatusCode::INTERNAL_SERVER_ERROR
    })?;
    Ok(Json(CameraStatus { camera_on: false }))
}

// ── GET /api/camera/stream ──

pub async fn stream(
    State(s): State<Arc<AppState>>,
) -> Response<Body> {
    let mut rx = s.camera.subscribe();

    let stream = async_stream::stream! {
        // 发送第一帧（如果有）
        {
            let jpeg = rx.borrow_and_update().clone();
            if !jpeg.is_empty() {
                yield mjpeg_part(&jpeg);
            }
        }
        // 防止 bufferbloat：限制 HTTP 端最大 ~10fps。
        // 摄像头 15fps（67ms/帧）→ 如果 WiFi 慢，TCP 发送缓冲积压 → 每帧
        // yield 阻塞越来越长 → "越来越卡"。这里每 100ms 最多发一帧，给 TCP
        // 缓冲足够的排空时间。
        let min_interval = tokio::time::Duration::from_millis(100);
        let mut last_send = tokio::time::Instant::now();
        loop {
            if rx.changed().await.is_err() {
                break;
            }
            // drain to latest
            let mut jpeg = rx.borrow_and_update().clone();
            while rx.has_changed().unwrap_or(false) {
                jpeg = rx.borrow_and_update().clone();
            }
            // 距上一帧不足 100ms 则跳过，等下一帧
            if last_send.elapsed() < min_interval {
                continue;
            }
            if !jpeg.is_empty() {
                yield mjpeg_part(&jpeg);
                last_send = tokio::time::Instant::now();
            }
        }
    };

    Response::builder()
        .header(
            header::CONTENT_TYPE,
            format!("multipart/x-mixed-replace; boundary={}", BOUNDARY),
        )
        .header(header::CACHE_CONTROL, "no-cache")
        .header("Connection", "close")
        .header("X-Accel-Buffering", "no")
        .body(Body::from_stream(stream))
        .unwrap()
}

// ── GET /api/camera/snapshot ──

pub async fn snapshot(
    State(s): State<Arc<AppState>>,
) -> Result<Json<serde_json::Value>, StatusCode> {
    let jpeg = s.camera.current_frame();
    if jpeg.is_empty() {
        return Err(StatusCode::SERVICE_UNAVAILABLE);
    }
    use base64::Engine as _;
    let b64 = base64::engine::general_purpose::STANDARD.encode(&jpeg);
    Ok(Json(serde_json::json!({
        "image": b64
    })))
}

// ── helpers ──

fn mjpeg_part(jpeg: &[u8]) -> Result<axum::body::Bytes, std::convert::Infallible> {
    let mut part = format!(
        "--{}\r\nContent-Type: image/jpeg\r\nContent-Length: {}\r\n\r\n",
        BOUNDARY,
        jpeg.len()
    )
    .into_bytes();
    part.extend_from_slice(jpeg);
    part.extend_from_slice(b"\r\n");
    Ok(axum::body::Bytes::from(part))
}
