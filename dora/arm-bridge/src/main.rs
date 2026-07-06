//! dora arm-bridge — 接收控制命令，驱动 ZP10S 舵机，回报状态
//!
//! 输入:  arm_cmd (JSON)    — 来自 web-server
//! 输出:  arm_status (JSON) — 每次命令后立刻回报（无周期 tick；机械臂无编码器）
//!
//! 硬件层：driver/ 屏蔽底层差异
//!   driver/dev.rs    — 开发环境，打印命令
//!   driver/zp10s.rs  — 真实硬件，UART 对接 ZP10S 控制板
//!
//! 命令分层：arm-bridge 只懂细粒度 set_angle / set_angles / torque / stop。
//! 高层 grab / release 由 web-server 读 arm_angles.json 后展开成 set_angle 序列
//! 再下发 —— 这样 arm_angles.json 的所有权留在 web-server（与现有 Python
//! src/arm_control/zl/zp10s/uart_control.py 行为一致）。

mod driver;

use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};

use arrow::array::AsArray;
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use driver::{create_driver, ArmDriver, ArmStatus, DriverConfig};
use eyre::{Context, Result};
use serde::Deserialize;

// ── 命令解析 ──
//
// 注意：`grab` / `release` 已被 web-server 展开成一串 set_angle，不会到这里。
// 这里只放细粒度原语。

#[derive(Debug, Deserialize)]
#[serde(tag = "command")]
enum ArmCmd {
    #[serde(rename = "set_angle")]
    SetAngle { servo_id: u8, angle: u16 },
    #[serde(rename = "set_angles")]
    SetAngles {
        /// key 形如 "servo0" / "servo1" / "servo2"，value 是角度
        angles: BTreeMap<String, u16>,
    },
    #[serde(rename = "torque")]
    Torque { on: bool },
    #[serde(rename = "stop")]
    Stop,
}

/// "servo0" / "servo1" / "servo2" → 0/1/2；其他返回 None
fn parse_servo_key(k: &str) -> Option<u8> {
    k.strip_prefix("servo")?.parse().ok()
}

fn publish_status(node: &Arc<tokio::sync::Mutex<DoraNode>>, status: &ArmStatus) {
    let bytes = serde_json::to_vec(status).unwrap_or_default();
    let mut n = match node.try_lock() {
        Ok(n) => n,
        Err(_) => {
            log::debug!("[arm-bridge] node busy, drop arm_status tick");
            return;
        }
    };
    let _ = n.send_output_bytes(
        "arm_status".into(),
        BTreeMap::new(),
        bytes.len(),
        &bytes,
    );
}

fn main() -> Result<()> {
    // 日志初始化：[logging] level 作为默认 filter；RUST_LOG 可临时覆盖
    //   level = "info"  → 仅生命周期 + 错误
    //   level = "debug" → + set_angle / TX 帧
    let config_for_log = dora_config::Config::load();
    env_logger::Builder::from_env(
        env_logger::Env::default().default_filter_or(&config_for_log.logging.level),
    )
    .format_timestamp_millis()
    .init();

    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(run()).unwrap_or_else(|e| log::error!("arm-bridge error: {:?}", e));
    Ok(())
}

async fn run() -> Result<()> {
    let config = dora_config::Config::load();

    let (node, mut events) = DoraNode::init_from_env()
        .wrap_err("Failed to init dora node")?;
    log::info!("[arm-bridge] Dora node initialized");

    let backend_name = config.arm.backend.clone();
    let driver_config = DriverConfig {
        backend: Some(config.arm.backend),
        port: Some(config.arm.port),
        baudrate: Some(config.arm.baudrate),
    };
    let driver: Arc<Mutex<Box<dyn ArmDriver>>> =
        Arc::new(Mutex::new(create_driver(&driver_config)));
    log::info!("[arm-bridge] Driver ready (backend={})", backend_name);

    // 启动时把"未知"状态先发一份，避免前端一直拿不到字段
    let initial_status = driver.lock().unwrap().status();
    let node_for_init = Arc::new(tokio::sync::Mutex::new(node));
    publish_status(&node_for_init, &initial_status);

    // 事件循环
    while let Some(event) = events.next().await {
        match event {
            Event::Input { id, data, .. } if id.to_string() == "arm_cmd" => {
                let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
                let cmd: ArmCmd = match serde_json::from_slice(uint8_arr.values()) {
                    Ok(c) => c,
                    Err(e) => {
                        log::warn!("[arm-bridge] parse arm_cmd: {:?}", e);
                        continue;
                    }
                };

                let mut d = driver.lock().unwrap();
                match cmd {
                    ArmCmd::SetAngle { servo_id, angle } => {
                        d.set_angle(servo_id, angle);
                    }
                    ArmCmd::SetAngles { angles } => {
                        let moves: Vec<(u8, u16)> = angles
                            .iter()
                            .filter_map(|(k, v)| match parse_servo_key(k) {
                                Some(id) => Some((id, *v)),
                                None => {
                                    log::warn!("[arm-bridge] skip invalid servo key: {}", k);
                                    None
                                }
                            })
                            .collect();
                        if moves.is_empty() {
                            log::warn!("[arm-bridge] set_angles with no valid keys, ignored");
                            continue;
                        }
                        d.set_angles(&moves);
                    }
                    ArmCmd::Torque { on } => {
                        if on {
                            d.restore_torque();
                        } else {
                            d.release_torque();
                        }
                    }
                    ArmCmd::Stop => {
                        d.stop();
                    }
                }
                drop(d);

                // 每次命令后立刻 publish 最新状态
                let status = driver.lock().unwrap().status();
                publish_status(&node_for_init, &status);
            }
            Event::Stop(cause) => {
                log::info!("[arm-bridge] Stop: {:?}, exiting", cause);
                break;
            }
            _ => {}
        }
    }

    driver.lock().unwrap().stop();
    log::info!("[arm-bridge] Shutdown");
    Ok(())
}