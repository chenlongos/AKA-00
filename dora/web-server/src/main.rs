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

use std::io::BufReader;
use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use arrow::array::AsArray;
use dora_node_api::{DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use tokio::net::TcpListener;
use tokio::sync::Mutex;
use axum_server;
use tower_http::services::{ServeDir, ServeFile};

use services::arm::ArmService;
use services::camera::CameraService;
use services::demo::DemoService;
use services::motor::MotorService;

/// 加载/生成自签 TLS 证书 → rustls::ServerConfig。
///
/// - 缺 cert/key 时用 rcgen 即时生成（CN=AKA-00 + localhost，3650 天）
/// - 路径默认 <DORA_HOME>/cert.pem + key.pem，可用 APP_CERT_PATH / APP_KEY_PATH 覆盖
/// - 与 python run.py 的 ensure_cert() 行为一致
async fn setup_tls(cert_path: &Path, key_path: &Path) -> Result<rustls::ServerConfig> {
    if !cert_path.exists() || !key_path.exists() {
        log::info!(
            "[web-server] generating self-signed cert (CN=AKA-00, 3650d) at {} + {}",
            cert_path.display(), key_path.display()
        );
        if let Some(parent) = cert_path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        // rcgen 0.13 API: generate_simple_self_signed(Vec<String>) -> CertifiedKey
        let ck = rcgen::generate_simple_self_signed(vec![
            "AKA-00".into(),
            "localhost".into(),
        ])
        .map_err(|e| eyre::eyre!("rcgen: {e}"))?;
        std::fs::write(cert_path, ck.cert.pem())?;
        std::fs::write(key_path, ck.key_pair.serialize_pem())?;
    }

    let cert_pem = std::fs::read(cert_path)
        .wrap_err_with(|| format!("read cert {}", cert_path.display()))?;
    let key_pem = std::fs::read(key_path)
        .wrap_err_with(|| format!("read key {}", key_path.display()))?;

    let certs: Vec<_> = rustls_pemfile::certs(&mut BufReader::new(cert_pem.as_slice()))
        .collect::<Result<_, _>>()
        .wrap_err("parse cert PEM")?;

    let key = rustls_pemfile::pkcs8_private_keys(&mut BufReader::new(key_pem.as_slice()))
        .next()
        .ok_or_else(|| eyre::eyre!("no private key in {}", key_path.display()))?
        .wrap_err("parse key PEM")?;

    let config = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(certs, rustls::pki_types::PrivateKeyDer::Pkcs8(key.secret_pkcs8_der().to_vec().into()))
        .map_err(|e| eyre::eyre!("rustls config: {e}"))?;
    Ok(config)
}

// HTTPS accept-loop 由 axum_server 内部处理；这里不再需要手写 accept。
// TlsAcceptor 从 use 中移除。

/// 合并所有服务的状态类型（axum 只允许一次 with_state）
#[derive(Clone)]
pub struct AppState {
    pub camera: Arc<CameraService>,
    pub motor: Arc<MotorService>,
    pub arm: Arc<ArmService>,
    pub demo: Arc<DemoService>,
}

fn main() -> Result<()> {
    // 日志：[logging] level 作为默认 filter；RUST_LOG 可临时覆盖
    let config_for_log = dora_config::Config::load();
    env_logger::Builder::from_env(
        env_logger::Env::default().default_filter_or(&config_for_log.logging.level),
    )
    .format_timestamp_millis()
    .init();

    // ── rustls 0.23+ 不再自动选 crypto provider ──
    // `ring` 已在 Cargo.toml 启用，这里再显式 install_default 兜底：
    // 万一上游 feature 解析变了（feature 合并、依赖图变化），这里至少给一个清晰错误，
    // 而不是等第一个 ServerConfig::builder() 触发深处的 panic。
    // install_default() 在已 install 时返回 Err，无害，吞掉即可。
    let _ = rustls::crypto::ring::default_provider().install_default();

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
        motor: Arc::new(MotorService::new(dora.clone())),
        arm: Arc::new(ArmService::new(dora.clone(), config.arm.clone())),
        demo: Arc::new(DemoService::new(dora)),
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
        .merge(routes::arm::router())
        .merge(routes::wifi::router())
        .merge(routes::demo::router())
        .merge(routes::system::router())
        .fallback_service(
            ServeDir::new(&static_dir)
                .fallback(ServeFile::new(static_dir.join("index.html")))
        )
        .with_state(state.clone());

    // ── 启动 HTTP（端口 80，来自 config.toml [web] port）──
    let http_addr = SocketAddr::from(([0, 0, 0, 0], config.web.port));
    let http_listener = TcpListener::bind(http_addr).await?;
    log::info!("[web-server] Listening on http://0.0.0.0:{}", config.web.port);
    let http_app = app.clone();
    let http_handle = tokio::spawn(async move {
        if let Err(e) = axum::serve(http_listener, http_app).await {
            log::error!("[web-server] http server: {e}");
        }
    });

    // ── 启动 HTTPS（端口 443，跟 python run.py 行为对齐）──
    // 缺证书用 rcgen 即时生成；路径在 DORA_HOME 下，与 python 一致 (APP_CERT_PATH / APP_KEY_PATH 可覆盖).
    let dora_home = std::env::var("DORA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/root/AKA-00"));
    let cert_path = std::env::var("APP_CERT_PATH")
        .map(PathBuf::from)
        .unwrap_or_else(|_| dora_home.join("cert.pem"));
    let key_path = std::env::var("APP_KEY_PATH")
        .map(PathBuf::from)
        .unwrap_or_else(|_| dora_home.join("key.pem"));

    let https_app = app.clone().into_make_service();
    let https_handle = match setup_tls(&cert_path, &key_path).await {
        Ok(config) => {
            let https_addr = SocketAddr::from(([0, 0, 0, 0], 443));
            // axum_server::RustlsConfig 把 rustls::ServerConfig 包一层，
            // 内部做 non-blocking + accept loop，跟 python 同语义。
            let rustls_cfg = axum_server::tls_rustls::RustlsConfig::from_config(std::sync::Arc::new(config));
            let handle = tokio::spawn(async move {
                if let Err(e) = axum_server::bind_rustls(https_addr, rustls_cfg)
                    .serve(https_app)
                    .await
                {
                    log::warn!("[web-server] https server exited: {e}");
                } else {
                    log::info!("[web-server] HTTPS shut down cleanly");
                }
            });
            log::info!(
                "[web-server] Listening on https://0.0.0.0:443 (cert: {})",
                cert_path.display()
            );
            Some(handle)
        }
        Err(e) => {
            log::warn!("[web-server] HTTPS disabled (cert setup failed: {e:#})");
            None
        }
    };

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

                        // 从 robot_state 提取 arm 字段（state-node 已聚合 arm_status）。
                        // arm_angles 是 BTreeMap<u8, u16>，JSON 序列化后 key 是字符串，
                        // 这里把它转回 u8 key 再灌进 ArmService 缓存。
                        let mut angles = std::collections::BTreeMap::new();
                        if let Some(obj) = v.get("arm_angles").and_then(|x| x.as_object()) {
                            for (k, val) in obj {
                                if let (Ok(id), Some(a)) =
                                    (k.parse::<u8>(), val.as_u64())
                                {
                                    angles.insert(id, a as u16);
                                }
                            }
                        }
                        let torque = v
                            .get("arm_torque")
                            .and_then(|x| x.as_str())
                            .unwrap_or("unknown")
                            .to_string();
                        let last_action = v
                            .get("arm_last_action")
                            .and_then(|x| x.as_str())
                            .unwrap_or("")
                            .to_string();
                        state.arm.update_status(services::arm::ArmStatus {
                            angles,
                            torque,
                            last_action,
                        });

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

    http_handle.abort();
    if let Some(h) = https_handle { h.abort(); }
    log::info!("[web-server] Shutdown");
    Ok(())
}
