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
use tower_http::services::{ServeDir, ServeFile};

use services::camera::CameraService;
use services::motor::MotorService;

/// 合并所有服务的状态类型（axum 只允许一次 with_state）
#[derive(Clone)]
pub struct AppState {
    pub camera: Arc<CameraService>,
    pub motor: Arc<MotorService>,
}

fn main() -> Result<()> {
    // 日志：[logging] level 作为默认 filter；RUST_LOG 可临时覆盖
    let config_for_log = dora_config::Config::load();
    env_logger::Builder::from_env(
        env_logger::Env::default().default_filter_or(&config_for_log.logging.level),
    )
    .format_timestamp_millis()
    .init();

    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(run())
        .unwrap_or_else(|e| log::error!("web-server error: {:?}", e));
    Ok(())
}

async fn run() -> Result<()> {
    // ── 读取配置 ──
    let config = dora_config::Config::load();

    // ── dora 初始化 ──
    let (node, mut events) = DoraNode::init_from_env()
        .wrap_err("Failed to init dora node")?;
    log::info!("[web-server] Dora node initialized");

    let dora = Arc::new(Mutex::new(node));

    // ── 服务层 ──
    let state = Arc::new(AppState {
        camera: Arc::new(CameraService::new(dora.clone(), config.camera.clone())),
        motor: Arc::new(MotorService::new(dora, config.chassis.clone())),
    });

    // ── HTTP 路由 ──
    // 从二进制位置推算:  bin/web-server -> ../static
    // 部署时: /root/dora-riscv64/bin/web-server -> /root/dora-riscv64/static
    // 开发时: target/release/web-server -> CARGO_MANIFEST_DIR/static
    let static_dir = std::env::current_exe()
        .ok()
        .and_then(|exe| {
            exe.parent()          // bin/
                .and_then(|p| p.parent())  // $DORA_HOME/
                .map(|root| root.join("static"))
        })
        .filter(|d| d.exists())
        .unwrap_or_else(|| {
            std::path::PathBuf::from(concat!(env!("CARGO_MANIFEST_DIR"), "/static"))
        });
    log::info!("[web-server] Static files: {}", static_dir.display());

    let app = routes::camera::router()
        .merge(routes::control::router())
        .merge(routes::motor::router())
        .fallback_service(
            ServeDir::new(&static_dir)
                .fallback(ServeFile::new(static_dir.join("index.html")))
        )
        .with_state(state.clone());

    // ── 启动 HTTP ──
    let addr = SocketAddr::from(([0, 0, 0, 0], config.web.port));
    let listener = tokio::net::TcpListener::bind(addr).await?;
    log::info!("[web-server] Listening on http://0.0.0.0:{}", config.web.port);

    let server_handle = tokio::spawn(async { axum::serve(listener, app).await.unwrap() });

    // ── dora 事件循环 ──
    while let Some(event) = events.next().await {
        match event {
            Event::Input { id, data, .. } => match id.to_string().as_str() {
                "image" => state.camera.push_frame(&data),
                "robot_state" => {
                    let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
                    if let Ok(v) = serde_json::from_slice::<serde_json::Value>(uint8_arr.values()) {
                        // state-node 已是 m/s（Python RobotStatus 字段名）
                        state.motor.update_speed(
                            v["left_speed"].as_f64().unwrap_or(0.0) as f32,
                            v["right_speed"].as_f64().unwrap_or(0.0) as f32,
                        );
                        // 注：camera_on 由 CameraService 自己管理（open/close API）
                        // 注：left_target / gripper_status / timestamp_ms 等暂未用到
                    }
                }
                _ => {}
            },
            Event::Stop(cause) => {
                log::info!("[web-server] Stop: {:?}, exiting", cause);
                break;
            }
            _ => {}
        }
    }

    server_handle.abort();
    log::info!("[web-server] Shutdown");
    Ok(())
}
