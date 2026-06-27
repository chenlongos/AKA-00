//! dora web-server — 桥接 dora dataflow 和浏览器前端
//!
//! 架构（仿 app/ 的 routes + services 分层）：
//!   main.rs          — 应用启动、dora 事件循环
//!   routes/camera.rs — HTTP 路由（薄层，参数解析 + 序列化）
//!   services/camera  — 业务逻辑（帧广播、设备控制、JPEG 编码）

mod routes;
mod services;

use std::net::SocketAddr;
use std::sync::Arc;

use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use tower_http::services::ServeDir;

use services::camera::CameraService;

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

    // ── 服务层 ──
    let camera_svc = Arc::new(CameraService::new(node));

    // ── HTTP 路由 ──
    let static_dir = concat!(env!("CARGO_MANIFEST_DIR"), "/static");
    println!("[web-server] Static files: {}", static_dir);

    let app = routes::camera::router()
        .fallback_service(ServeDir::new(static_dir))
        .with_state(camera_svc.clone());

    // ── 启动 HTTP ──
    let addr = SocketAddr::from(([0, 0, 0, 0], 8080));
    let listener = tokio::net::TcpListener::bind(addr).await?;
    println!("[web-server] Listening on http://0.0.0.0:8080");

    let server_handle = tokio::spawn(async { axum::serve(listener, app).await.unwrap() });

    // ── dora 事件循环 ──
    while let Some(event) = events.next().await {
        match event {
            Event::Input { id, data, .. } if id.to_string() == "image" => {
                camera_svc.push_frame(&data);
            }
            Event::Stop(cause) => {
                println!("[web-server] Stop: {:?}, exiting", cause);
                break;
            }
            _ => {}
        }
    }

    server_handle.abort();
    println!("[web-server] Shutdown");
    Ok(())
}
