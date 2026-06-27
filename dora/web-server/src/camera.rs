//! Camera API handlers — 路径和响应格式完全匹配 frontend/src/api.ts
//!
//! 不做任何前端改动，只需将 Vite build 产物放到 static/ 即可使用。

use std::collections::BTreeMap;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use axum::{
    body::Body,
    extract::{Query, State},
    http::{header, Response, StatusCode},
    Json,
};
use serde::Deserialize;

use crate::AppState;

const BOUNDARY: &str = "dora-frame";

// ── 响应类型 ──

/// 前端期望的 camera status / open / close 响应格式
#[derive(serde::Serialize)]
pub(crate) struct CameraStatus {
    pub camera_on: bool,
}

// ── GET /api/camera/status ──

pub async fn status(State(state): State<Arc<AppState>>) -> Json<CameraStatus> {
    Json(CameraStatus {
        camera_on: state.camera_active.load(Ordering::Relaxed),
    })
}

// ── POST /api/camera/open ──

pub async fn open(State(state): State<Arc<AppState>>) -> Result<Json<CameraStatus>, StatusCode> {
    if state.camera_active.swap(true, Ordering::Relaxed) {
        // 已经开启，直接返回
        return Ok(Json(CameraStatus { camera_on: true }));
    }

    let mut node = state.node.lock().await;
    node.send_output_bytes("control".into(), BTreeMap::new(), 5, b"start")
        .map_err(|e| {
            eprintln!("[web-server] send start failed: {:?}", e);
            StatusCode::INTERNAL_SERVER_ERROR
        })?;

    println!("[web-server] 📷 camera on");
    Ok(Json(CameraStatus { camera_on: true }))
}

// ── POST /api/camera/close ──

pub async fn close(State(state): State<Arc<AppState>>) -> Result<Json<CameraStatus>, StatusCode> {
    if !state.camera_active.swap(false, Ordering::Relaxed) {
        // 已经关闭
        return Ok(Json(CameraStatus { camera_on: false }));
    }

    let mut node = state.node.lock().await;
    node.send_output_bytes("control".into(), BTreeMap::new(), 4, b"stop")
        .map_err(|e| {
            eprintln!("[web-server] send stop failed: {:?}", e);
            StatusCode::INTERNAL_SERVER_ERROR
        })?;

    println!("[web-server] ⏸  camera off");
    Ok(Json(CameraStatus { camera_on: false }))
}

// ── GET /api/camera/stream?fps=N ──

#[derive(Deserialize, Default)]
pub struct StreamParams {
    fps: Option<u32>,
}

pub async fn stream(
    State(state): State<Arc<AppState>>,
    Query(params): Query<StreamParams>,
) -> Response<Body> {
    let _fps = params.fps.unwrap_or(30);
    let mut rx = state.frame_tx.subscribe();

    let stream = async_stream::stream! {
        // 立即发送当前帧
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

/// GET /api/camera/snapshot — 当前帧的 base64 JPEG（兼容老 API）
pub async fn snapshot(State(state): State<Arc<AppState>>) -> Result<Json<serde_json::Value>, StatusCode> {
    let jpeg = state.frame_tx.borrow().clone();
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
