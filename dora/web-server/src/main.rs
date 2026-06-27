use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use arrow::array::AsArray;
use axum::{
    body::Body,
    extract::State,
    http::{header, Response, StatusCode},
    routing::{get, post},
    Router,
};
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use tokio::sync::{watch, Mutex};
use tower_http::services::ServeDir;

const SRC_WIDTH: u32 = 640;
const SRC_HEIGHT: u32 = 480;
const JPEG_QUALITY: u8 = 70;
const BOUNDARY: &str = "dora-frame";

/// 共享状态
struct AppState {
    frame_tx: watch::Sender<Vec<u8>>,
    camera_active: Arc<AtomicBool>,
    node: Arc<Mutex<DoraNode>>,
}

fn main() -> Result<()> {
    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(async {
        if let Err(e) = run().await {
            eprintln!("web-server error: {:?}", e);
        }
    });
    Ok(())
}

async fn run() -> Result<()> {
    // ── 初始化 dora 节点 ──
    let (node, mut events) = DoraNode::init_from_env()
        .wrap_err("Failed to init dora node")?;
    println!("[web-server] Dora node initialized");

    // ── 启动 HTTP 服务 ──
    let (frame_tx, _) = watch::channel(Vec::new());
    let camera_active = Arc::new(AtomicBool::new(false));
    let state = Arc::new(AppState {
        frame_tx: frame_tx.clone(),
        camera_active: camera_active.clone(),
        node: Arc::new(Mutex::new(node)),
    });

    let static_dir = concat!(env!("CARGO_MANIFEST_DIR"), "/static");
    println!("[web-server] Serving static files from {}", static_dir);

    let app = Router::new()
        .route("/stream", get(stream_handler))
        .route("/api/status", get(status_handler))
        .route("/api/camera/open", post(camera_open))
        .route("/api/camera/close", post(camera_close))
        .fallback_service(ServeDir::new(static_dir))
        .with_state(state.clone());

    let addr = SocketAddr::from(([0, 0, 0, 0], 8080));
    let listener = tokio::net::TcpListener::bind(addr).await?;
    println!("[web-server] HTTP server listening on http://0.0.0.0:8080");

    let server_handle = tokio::spawn(async move {
        axum::serve(listener, app).await.unwrap();
    });

    // ── dora 事件循环 ──
    let mut frame_count: u64 = 0;

    while let Some(event) = events.next().await {
        match event {
            Event::Input { id, data, .. } if id.to_string() == "image" => {
                // 仅当摄像头开启时才推流
                if !camera_active.load(Ordering::Relaxed) {
                    continue;
                }

                let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
                let rgb_bytes = uint8_arr.values();

                let expected_len = (SRC_WIDTH * SRC_HEIGHT * 3) as usize;
                if rgb_bytes.len() < expected_len {
                    continue;
                }

                let mut jpeg = Vec::new();
                let mut encoder =
                    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut jpeg, JPEG_QUALITY);
                if encoder
                    .encode(
                        &rgb_bytes[..expected_len],
                        SRC_WIDTH,
                        SRC_HEIGHT,
                        image::ExtendedColorType::Rgb8,
                    )
                    .is_ok()
                {
                    frame_tx.send_replace(jpeg);
                    frame_count += 1;
                    if frame_count % 30 == 0 {
                        println!("[web-server] {} frames served", frame_count);
                    }
                }
            }
            Event::Stop(cause) => {
                println!("[web-server] Stop: {:?}, exiting", cause);
                break;
            }
            _ => {}
        }
    }

    server_handle.abort();
    println!("[web-server] Shutting down ({} frames)", frame_count);
    Ok(())
}

// ── 摄像头控制 API ──

/// POST /api/camera/open — 开启摄像头采集
async fn camera_open(State(state): State<Arc<AppState>>) -> Result<String, StatusCode> {
    if state.camera_active.load(Ordering::Relaxed) {
        return Ok("already on".into());
    }

    let mut node = state.node.lock().await;
    node.send_output_bytes(
        "control".into(),
        std::collections::BTreeMap::new(),
        5,
        b"start",
    )
    .map_err(|e| {
        eprintln!("[web-server] Failed to send start: {:?}", e);
        StatusCode::INTERNAL_SERVER_ERROR
    })?;

    state.camera_active.store(true, Ordering::Relaxed);
    println!("[web-server] 📷 camera on");
    Ok("ok".into())
}

/// POST /api/camera/close — 关闭摄像头采集
async fn camera_close(State(state): State<Arc<AppState>>) -> Result<String, StatusCode> {
    if !state.camera_active.load(Ordering::Relaxed) {
        return Ok("already off".into());
    }

    let mut node = state.node.lock().await;
    // 消耗掉 watch channel 里残留的旧帧
    state.camera_active.store(false, Ordering::Relaxed);

    node.send_output_bytes(
        "control".into(),
        std::collections::BTreeMap::new(),
        4,
        b"stop",
    )
    .map_err(|e| {
        eprintln!("[web-server] Failed to send stop: {:?}", e);
        StatusCode::INTERNAL_SERVER_ERROR
    })?;

    println!("[web-server] ⏸  camera off");
    Ok("ok".into())
}

// ── 查询 ──

/// GET /api/status — 健康检查
async fn status_handler(State(state): State<Arc<AppState>>) -> axum::Json<serde_json::Value> {
    let has_frame = !state.frame_tx.borrow().is_empty();
    let camera_on = state.camera_active.load(Ordering::Relaxed);
    axum::Json(serde_json::json!({
        "status": if has_frame && camera_on { "streaming" } else if camera_on { "waiting" } else { "stopped" },
        "camera_on": camera_on,
        "resolution": format!("{}x{}", SRC_WIDTH, SRC_HEIGHT),
        "service": "dora-web-server"
    }))
}

// ── MJPEG 流 ──

/// GET /stream — MJPEG 流，浏览器 <img> 直接消费
async fn stream_handler(State(state): State<Arc<AppState>>) -> Response<Body> {
    let mut rx = state.frame_tx.subscribe();

    let stream = async_stream::stream! {
        // 先发送当前帧（如果有）
        {
            let jpeg = rx.borrow_and_update().clone();
            if !jpeg.is_empty() {
                yield build_mjpeg_part(&jpeg);
            }
        }

        loop {
            match rx.changed().await {
                Ok(()) => {
                    let jpeg = rx.borrow_and_update().clone();
                    if jpeg.is_empty() {
                        continue;
                    }
                    yield build_mjpeg_part(&jpeg);
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

fn build_mjpeg_part(jpeg: &[u8]) -> Result<axum::body::Bytes, std::convert::Infallible> {
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
