//! dora web-server — 桥接 dora dataflow 和浏览器前端
//!
//! 职责：接收摄像头帧 → MJPEG 推流 + 提供 HTTP API（兼容现有前端）
//! API 契约完全匹配 frontend/src/api.ts 的调用，不修改前端代码。
//!
//! 路由：
//!   GET  /api/camera/status      → { camera_on: bool }
//!   POST /api/camera/open         → { camera_on: true }  + dora "start"
//!   POST /api/camera/close        → { camera_on: false } + dora "stop"
//!   GET  /api/camera/stream?fps=N → MJPEG multipart stream
//!   GET  /api/camera/snapshot     → { image: "base64..." }
//!   /*                            → static/ 目录（React SPA）

mod camera;

use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use arrow::array::AsArray;
use axum::{routing::get, Router};
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use tokio::sync::{watch, Mutex};
use tower_http::services::ServeDir;

const SRC_WIDTH: u32 = 640;
const SRC_HEIGHT: u32 = 480;
const JPEG_QUALITY: u8 = 70;

/// 全局共享状态
pub struct AppState {
    /// MJPEG 帧广播：camera 事件循环写入，每个 /stream 客户端订阅
    pub frame_tx: watch::Sender<Vec<u8>>,
    /// 摄像头开关状态
    pub camera_active: Arc<AtomicBool>,
    /// dora 节点句柄，用于发送 control 消息
    pub node: Arc<Mutex<DoraNode>>,
}

fn main() -> Result<()> {
    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(run()).unwrap_or_else(|e| eprintln!("web-server error: {:?}", e));
    Ok(())
}

async fn run() -> Result<()> {
    // ── dora 初始化 ──
    let (node, mut events) = DoraNode::init_from_env()
        .wrap_err("Failed to init dora node")?;
    println!("[web-server] Dora node initialized");

    // ── 共享状态 ──
    let (frame_tx, _rx) = watch::channel(Vec::new());
    // _rx 保持存活，确保 watch channel 不会因为没有 receiver 而拒绝 send
    let _keepalive = _rx;

    let state = Arc::new(AppState {
        frame_tx: frame_tx.clone(),
        camera_active: Arc::new(AtomicBool::new(false)),
        node: Arc::new(Mutex::new(node)),
    });

    // ── HTTP 路由 ──
    let static_dir = concat!(env!("CARGO_MANIFEST_DIR"), "/static");
    println!("[web-server] Static files: {}", static_dir);

    let app = Router::new()
        // Camera API — 路径和响应格式完全匹配 frontend/src/api.ts
        .route("/api/camera/status", get(camera::status))
        .route("/api/camera/open", axum::routing::post(camera::open))
        .route("/api/camera/close", axum::routing::post(camera::close))
        .route("/api/camera/stream", get(camera::stream))
        .route("/api/camera/snapshot", get(camera::snapshot))
        // SPA 回退 — 前端 build 产物
        .fallback_service(ServeDir::new(static_dir))
        .with_state(state.clone());

    // ── 启动 HTTP ──
    let addr = SocketAddr::from(([0, 0, 0, 0], 8080));
    let listener = tokio::net::TcpListener::bind(addr).await?;
    println!("[web-server] Listening on http://0.0.0.0:8080");

    let server_handle = tokio::spawn(async { axum::serve(listener, app).await.unwrap() });

    // ── dora 事件循环：接收摄像头帧 → JPEG → watch channel ──
    let mut frame_count: u64 = 0;
    while let Some(event) = events.next().await {
        if let Event::Input { id, data, .. } = event {
            if id.to_string() != "image" {
                continue;
            }

            if !state.camera_active.load(Ordering::Relaxed) {
                continue;
            }

            let jpeg = match rgb_to_jpeg(&data) {
                Some(j) => j,
                None => continue,
            };

            state.frame_tx.send_replace(jpeg);
            frame_count += 1;
            if frame_count % 90 == 0 {
                println!("[web-server] {} frames", frame_count);
            }
        } else if let Event::Stop(cause) = event {
            println!("[web-server] Stop: {:?}, exiting", cause);
            break;
        }
    }

    server_handle.abort();
    println!("[web-server] Shutdown ({} frames)", frame_count);
    Ok(())
}

/// 从 dora Arrow 数据提取 RGB 并编码 JPEG，失败返回 None
fn rgb_to_jpeg(data: &dora_node_api::ArrowData) -> Option<Vec<u8>> {
    let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
    let rgb = uint8_arr.values();

    let expected = (SRC_WIDTH * SRC_HEIGHT * 3) as usize;
    if rgb.len() < expected {
        return None;
    }

    let mut buf = Vec::new();
    let mut enc = image::codecs::jpeg::JpegEncoder::new_with_quality(&mut buf, JPEG_QUALITY);
    enc.encode(&rgb[..expected], SRC_WIDTH, SRC_HEIGHT, image::ExtendedColorType::Rgb8)
        .ok()?;
    Some(buf)
}
