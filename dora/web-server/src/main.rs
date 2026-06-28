//! dora web-server — 桥接 dora dataflow 和浏览器前端
//!
//! 架构（仿 app/ 的 routes + services 分层）：
//!   main.rs            — 应用启动、dora 事件循环
//!   routes/camera.rs   — 摄像头路由（MJPEG、开关、快照）
//!   routes/control.rs  — 控制路由（REST + WebSocket 摇杆/JSON）
//!   routes/motor.rs    — 电机路由（状态、直接控制）
//!   services/camera.rs — 摄像头服务（帧广播、设备开关）
//!   services/motor.rs  — 电机服务（命令转发、状态管理）

mod routes;
mod services;

use std::net::SocketAddr;
use std::sync::Arc;

use arrow::array::AsArray;
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use tokio::sync::Mutex;
use tower_http::services::ServeDir;

use services::camera::CameraService;
use services::motor::MotorService;

/// 合并所有服务的状态类型（axum 只允许一次 with_state）
#[derive(Clone)]
pub struct AppState {
    pub camera: Arc<CameraService>,
    pub motor: Arc<MotorService>,
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

    let dora = Arc::new(Mutex::new(node));

    // ── 服务层 ──
    let state = Arc::new(AppState {
        camera: Arc::new(CameraService::new(dora.clone())),
        motor: Arc::new(MotorService::new(dora)),
    });

    // ── HTTP 路由 ──
    let static_dir = concat!(env!("CARGO_MANIFEST_DIR"), "/static");
    println!("[web-server] Static files: {}", static_dir);

    let app = routes::camera::router()
        .merge(routes::control::router())
        .merge(routes::motor::router())
        .fallback_service(ServeDir::new(static_dir))
        .with_state(state.clone());

    // ── 启动 HTTP ──
    let addr = SocketAddr::from(([0, 0, 0, 0], 8080));
    let listener = tokio::net::TcpListener::bind(addr).await?;
    println!("[web-server] Listening on http://0.0.0.0:8080");

    let server_handle = tokio::spawn(async { axum::serve(listener, app).await.unwrap() });

    // ── dora 事件循环 ──
    while let Some(event) = events.next().await {
        match event {
            Event::Input { id, data, .. } => match id.to_string().as_str() {
                "image" => state.camera.push_frame(&data),
                "robot_state" => {
                    let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
                    if let Ok(v) = serde_json::from_slice::<serde_json::Value>(uint8_arr.values()) {
                        // 电机状态
                        state.motor.update_rpm(
                            v["motor_left_rpm"].as_i64().unwrap_or(0) as i32,
                            v["motor_right_rpm"].as_i64().unwrap_or(0) as i32,
                        );
                        // 摄像头状态
                        if let Some(on) = v["camera_on"].as_bool() {
                            state.camera.set_active(on);
                        }
                    }
                }
                _ => {}
            },
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
