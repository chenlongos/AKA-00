//! Camera HTTP 路由 —— 薄层，只做参数解析和序列化，业务逻辑委托给 CameraService
//!
//! 对应 Python 项目 `app/routes/camera.py` 的职责。
//! API 契约完全匹配 frontend/src/api.ts，不修改前端代码。

use std::sync::Arc;

use axum::{
    body::Body,
    extract::{Query, State},
    http::{header, Response, StatusCode},
    Json,
};
use serde::Deserialize;

use crate::services::camera::CameraService;

const BOUNDARY: &str = "dora-frame";

// ── 响应类型 ──

#[derive(serde::Serialize)]
pub struct CameraStatus {
    pub camera_on: bool,
}

// ── GET /api/camera/status ──

pub async fn status(State(svc): State<Arc<CameraService>>) -> Json<CameraStatus> {
    Json(CameraStatus {
        camera_on: svc.is_active(),
    })
}

// ── POST /api/camera/open ──

pub async fn open(State(svc): State<Arc<CameraService>>) -> Result<Json<CameraStatus>, StatusCode> {
    svc.open().await.map_err(|e| {
        eprintln!("[camera-route] open failed: {:?}", e);
        StatusCode::INTERNAL_SERVER_ERROR
    })?;
    Ok(Json(CameraStatus { camera_on: true }))
}

// ── POST /api/camera/close ──

pub async fn close(
    State(svc): State<Arc<CameraService>>,
) -> Result<Json<CameraStatus>, StatusCode> {
    svc.close().await.map_err(|e| {
        eprintln!("[camera-route] close failed: {:?}", e);
        StatusCode::INTERNAL_SERVER_ERROR
    })?;
    Ok(Json(CameraStatus { camera_on: false }))
}

// ── GET /api/camera/stream?fps=N ──

#[derive(Deserialize, Default)]
pub struct StreamParams {
    #[allow(dead_code)]
    fps: Option<u32>,
}

pub async fn stream(
    State(svc): State<Arc<CameraService>>,
    Query(_params): Query<StreamParams>,
) -> Response<Body> {
    let mut rx = svc.subscribe();

    let stream = async_stream::stream! {
        {
            let jpeg = rx.borrow_and_update().clone();
            if !jpeg.is_empty() {
                yield mjpeg_part(&jpeg);
            }
        }
        loop {
            match rx.changed().await {
                Ok(()) => {
                    let jpeg = rx.borrow_and_update().clone();
                    if !jpeg.is_empty() {
                        yield mjpeg_part(&jpeg);
                    }
                }
                Err(_) => break,
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
    State(svc): State<Arc<CameraService>>,
) -> Result<Json<serde_json::Value>, StatusCode> {
    let jpeg = svc.current_frame();
    if jpeg.is_empty() {
        return Err(StatusCode::SERVICE_UNAVAILABLE);
    }
    use base64::Engine as _;
    let b64 = base64::engine::general_purpose::STANDARD.encode(&jpeg);
    Ok(Json(serde_json::json!({
        "image": b64,
        "m": 0,
        "c": 0
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
