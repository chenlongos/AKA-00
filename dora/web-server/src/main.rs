use std::net::SocketAddr;
use std::sync::Arc;

use arrow::array::AsArray;
use axum::{
    body::Body,
    extract::State,
    http::{header, Response},
    routing::get,
    Router,
};
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use tokio::sync::watch;

const SRC_WIDTH: u32 = 640;
const SRC_HEIGHT: u32 = 480;
const JPEG_QUALITY: u8 = 70;
const BOUNDARY: &str = "dora-frame";

/// 应用共享状态：存最新的 JPEG 帧，所有 /stream 客户端从这里读
struct AppState {
    frame_tx: watch::Sender<Vec<u8>>,
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
    let (_node, mut events) = DoraNode::init_from_env()
        .wrap_err("Failed to init dora node")?;
    println!("[web-server] Dora node initialized");

    // ── 启动 HTTP 服务 ──
    let (frame_tx, _) = watch::channel(Vec::new());
    let state = Arc::new(AppState { frame_tx: frame_tx.clone() });

    let app = Router::new()
        .route("/", get(index_page))
        .route("/stream", get(stream_handler))
        .route("/api/status", get(status_handler))
        .with_state(state.clone());

    let addr = SocketAddr::from(([0, 0, 0, 0], 8080));
    let listener = tokio::net::TcpListener::bind(addr).await?;
    println!("[web-server] HTTP server listening on http://0.0.0.0:8080");

    // axum 在后台任务运行
    let server_handle = tokio::spawn(async move {
        axum::serve(listener, app).await.unwrap();
    });

    // ── dora 事件循环 ──
    let mut frame_count: u64 = 0;

    while let Some(event) = events.next().await {
        match event {
            Event::Input { id, data, .. } if id.to_string() == "image" => {
                // 从 Arrow 数组提取原始 RGB，编码为 JPEG
                let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
                let rgb_bytes = uint8_arr.values();

                let expected_len = (SRC_WIDTH * SRC_HEIGHT * 3) as usize;
                if rgb_bytes.len() < expected_len {
                    eprintln!("[web-server] Frame size mismatch: {} bytes (expected {})", rgb_bytes.len(), expected_len);
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
                    let _ = frame_tx.send(jpeg);
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

/// GET / — 简单的摄像头预览页面
async fn index_page() -> Response<Body> {
    let html = r#"<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>AKA-00 Camera</title>
<style>
  *{margin:0;padding:0;box-sizing:border-box}
  body{background:#111;display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;font-family:-apple-system,BlinkMacSystemFont,sans-serif}
  .container{position:relative;max-width:100vw;max-height:100vh}
  img{display:block;max-width:100vw;max-height:calc(100vh - 56px);object-fit:contain}
  .bar{display:flex;align-items:center;justify-content:space-between;padding:8px 16px;background:#1a1a2e;color:#e0e0e0;font-size:13px;width:100%}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#4f4;margin-right:6px;animation:pulse 2s infinite}
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
  .fps{color:#888}
</style>
</head>
<body>
  <div class="bar">
    <span><span class="dot" id="dot"></span><span id="status">connecting</span></span>
    <span class="fps" id="fps"></span>
  </div>
  <div class="container">
    <img id="cam" src="/stream" alt="camera"
         onload="document.getElementById('status').textContent='live';document.getElementById('dot').style.background='#4f4'"
         onerror="document.getElementById('status').textContent='offline';document.getElementById('dot').style.background='#f44'">
  </div>
</body>
</html>"#;

    Response::builder()
        .header(header::CONTENT_TYPE, "text/html; charset=utf-8")
        .body(Body::from(html))
        .unwrap()
}

/// GET /stream — MJPEG 流，浏览器 <img> 直接消费
async fn stream_handler(State(state): State<Arc<AppState>>) -> Response<Body> {
    let mut rx = state.frame_tx.subscribe();

    // 先让 receiver 跳过初始空值
    // 注意：watch::subscribe 返回当前值的 receiver，初始是空 vec

    let stream = async_stream::stream! {
        // 先立刻发送当前帧（如果有的话）
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
                Err(_) => break,  // sender dropped
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

/// 构造一个 MJPEG multipart 帧
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

/// GET /api/status — 健康检查
async fn status_handler(State(state): State<Arc<AppState>>) -> axum::Json<serde_json::Value> {
    let has_frame = !state.frame_tx.borrow().is_empty();
    axum::Json(serde_json::json!({
        "status": if has_frame { "streaming" } else { "waiting" },
        "resolution": format!("{}x{}", SRC_WIDTH, SRC_HEIGHT),
        "service": "dora-web-server"
    }))
}
