//! dora motor-bridge — 接收控制命令，驱动底盘电机，回报状态
//!
//! 输入:  motor_cmd (JSON)     — 来自 web-server
//! 输出:  motor_status (JSON)  — 每 200ms 回报真实 RPM
//!
//! 硬件层：driver/ 屏蔽底层差异
//!   driver/dev.rs     — 开发环境，打印命令
//!   driver/tt_pid.rs  — 真实硬件，UART 对接 ESP32-C3

mod driver;

use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};

use arrow::array::AsArray;
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use driver::{create_driver, DriverConfig, MotorDriver};
use eyre::{Context, Result};
use serde::Deserialize;

// ── 命令解析 ──

#[derive(Debug, Deserialize)]
#[serde(tag = "command")]
enum MotorCmd {
    #[serde(rename = "action")]
    Action { action: String, speed: u8, duration: u32 },
    #[serde(rename = "direct")]
    Direct { left: i32, right: i32, duration: u32 },
    #[serde(rename = "joystick")]
    Joystick { x: i8, y: i8 },
    #[serde(rename = "reinitialize")]
    Reinitialize,
    /// 闭环距离控制: {command:"move", direction:"forward", distance_mm:200, speed:50}
    #[serde(rename = "move")]
    Move { direction: String, distance_mm: f32, speed: u8 },
}

fn joystick_to_tank(x: i8, y: i8) -> (i32, i32) {
    let fy = (y as f32) / 127.0;
    let fx = (x as f32) / 127.0;
    let c = |v: f32| (v * 100.0).clamp(-100.0, 100.0) as i32;
    (c(fy + fx), c(fy - fx))
}

// ── 闭环距离控制 ──
const WHEEL_CIRCUMFERENCE_M: f32 = 0.19478; // π × 0.062m
const DISTANCE_POLL_MS: u64 = 50;

struct DistanceCtrl {
    target_m: f32,      // 目标距离(m)，正=前进
    traveled_m: f32,    // 已行驶距离(m)
}

impl DistanceCtrl {
    fn update(&mut self, left_rpm: i32, right_rpm: i32) -> bool {
        let avg_mps = (left_rpm as f32 + right_rpm as f32) / 2.0 / 60.0 * WHEEL_CIRCUMFERENCE_M;
        self.traveled_m += avg_mps * (DISTANCE_POLL_MS as f32 / 1000.0);
        self.traveled_m.abs() >= self.target_m.abs()
    }
}

fn main() -> Result<()> {
    // 日志初始化：[logging] level 作为默认 filter；RUST_LOG 可临时覆盖
    //   level = "info"  → 仅生命周期 + 错误
    //   level = "debug" → + set_speeds / GET_STATUS / TX-RX hex
    //   level = "trace" → + 内部解析细节
    let config_for_log = dora_config::Config::load();
    env_logger::Builder::from_env(
        env_logger::Env::default().default_filter_or(&config_for_log.logging.level),
    )
    .format_timestamp_millis()
    .init();

    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(run()).unwrap_or_else(|e| log::error!("motor-bridge error: {:?}", e));
    Ok(())
}

async fn run() -> Result<()> {
    let config = dora_config::Config::load();

    let (node, mut events) = DoraNode::init_from_env()
        .wrap_err("Failed to init dora node")?;
    log::info!("[motor-bridge] Dora node initialized");

    let backend_name = config.motor.backend.clone();
    let driver_config = DriverConfig {
        backend: Some(config.motor.backend),
        port: Some(config.motor.port),
        baudrate: Some(config.motor.baudrate),
        ppr: Some(config.motor.ppr),
    };
    let driver: Arc<Mutex<Box<dyn MotorDriver>>> =
        Arc::new(Mutex::new(create_driver(&driver_config)));
    log::info!("[motor-bridge] Driver ready (backend={})", backend_name);

    // 闭环距离控制状态
    let dist_ctrl: Arc<tokio::sync::Mutex<Option<DistanceCtrl>>> = Arc::new(tokio::sync::Mutex::new(None));

    // RPM 上报 + 距离闭环（50ms 间隔）
    let driver_rpt = driver.clone();
    let dist_rpt = dist_ctrl.clone();
    let node_rpt = Arc::new(tokio::sync::Mutex::new(node));
    let node_clone = node_rpt.clone();

    tokio::spawn(async move {
        let mut tick = tokio::time::interval(std::time::Duration::from_millis(DISTANCE_POLL_MS));
        loop {
            tick.tick().await;
            let driver_for_blocking = driver_rpt.clone();
            let (left, right) = match tokio::task::spawn_blocking(move || {
                driver_for_blocking.lock().unwrap().rpm()
            }).await {
                Ok(v) => v,
                Err(e) => { log::warn!("[motor-bridge] rpm: {:?}", e); (0, 0) }
            };

            // 距离闭环检查
            let mut ctrl = dist_rpt.lock().await;
            let reached = if let Some(ref mut d) = *ctrl {
                d.update(left, right)
            } else { false };
            if reached {
                log::info!("[motor-bridge] distance reached: {:.1}mm",
                    ctrl.as_ref().map(|d| d.traveled_m * 1000.0).unwrap_or(0.0));
                *ctrl = None;
                drop(ctrl);
                let driver_stop = driver_rpt.clone();
                tokio::task::spawn_blocking(move || { driver_stop.lock().unwrap().stop(); }).await.ok();
            } else {
                drop(ctrl);
            }

            let status = serde_json::json!({ "left_rpm": left, "right_rpm": right });
            let bytes = serde_json::to_vec(&status).unwrap_or_default();
            let mut node = node_clone.lock().await;
            let _ = node.send_output_bytes("motor_status".into(), BTreeMap::new(), bytes.len(), &bytes);
        }
    });

    // 事件循环
    while let Some(event) = events.next().await {
        match event {
            Event::Input { id, data, .. } if id.to_string() == "motor_cmd" => {
                let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
                let cmd: MotorCmd = match serde_json::from_slice(uint8_arr.values()) {
                    Ok(c) => c,
                    Err(e) => {
                        log::warn!("[motor-bridge] parse motor_cmd: {:?}", e);
                        continue;
                    }
                };

                // ── 无 duration 的即时指令（joystick/stop/direct 等）──
                // 串口 write 只需 <1ms，直接在当前 task 执行最快。
                // spawn_blocking 在单核 RISC-V 上线调度开销 5-20ms，摇杆 10Hz
                // 流量下延迟不可接受。
                let needs_duration = matches!(&cmd,
                    MotorCmd::Action { duration, .. } | MotorCmd::Direct { duration, .. }
                    if *duration > 0
                );

                if needs_duration {
                    // 有 duration：sleep 会阻塞，必须放 spawn_blocking
                    let driver_for_cmd = driver.clone();
                    let _ = tokio::task::spawn_blocking(move || {
                        let mut d = driver_for_cmd.lock().unwrap();
                        match cmd {
                            MotorCmd::Action { action, speed, duration } => {
                                let (l, r) = match action.as_str() {
                                    "up" => (speed as i32, speed as i32),
                                    "down" => (-(speed as i32), -(speed as i32)),
                                    "left" => (-(speed as i32), speed as i32),
                                    "right" => (speed as i32, -(speed as i32)),
                                    "stop" => (0, 0),
                                    _ => return,
                                };
                                d.set_speeds(l, r);
                                drop(d);
                                std::thread::sleep(std::time::Duration::from_millis(duration as u64));
                                driver_for_cmd.lock().unwrap().stop();
                            }
                            MotorCmd::Direct { left, right, duration } => {
                                d.set_speeds(left, right);
                                drop(d);
                                std::thread::sleep(std::time::Duration::from_millis(duration as u64));
                                driver_for_cmd.lock().unwrap().stop();
                            }
                            _ => {} // unreachable: needs_duration guards this
                        }
                    }).await;
                } else {
                    // 快速路径：直接执行（joystick / stop / reinitialize / duration=0）
                    let mut d = driver.lock().unwrap();
                    match cmd {
                        MotorCmd::Action { action, speed, .. } => {
                            let (l, r) = match action.as_str() {
                                "up" => (speed as i32, speed as i32),
                                "down" => (-(speed as i32), -(speed as i32)),
                                "left" => (-(speed as i32), speed as i32),
                                "right" => (speed as i32, -(speed as i32)),
                                "stop" => (0, 0),
                                _ => {
                                    log::warn!("[motor-bridge] unknown action: {}", action);
                                    (0, 0) // safe: stop
                                }
                            };
                            d.set_speeds(l, r);
                        }
                        MotorCmd::Direct { left, right, .. } => {
                            d.set_speeds(left, right);
                        }
                        MotorCmd::Joystick { x, y } => {
                            let (l, r) = joystick_to_tank(x, y);
                            d.set_speeds(l, r);
                        }
                        MotorCmd::Reinitialize => {
                            let reinit = d.reinitialize();
                            log::info!("[motor-bridge] reinitialize: {}", reinit);
                        }
                        MotorCmd::Move { direction, distance_mm, speed } => {
                            let (l, r) = match direction.as_str() {
                                "forward" => (speed as i32, speed as i32),
                                "backward" => (-(speed as i32), -(speed as i32)),
                                "left" => (-(speed as i32), speed as i32),
                                "right" => (speed as i32, -(speed as i32)),
                                other => {
                                    log::warn!("[motor-bridge] unknown move direction: {}", other);
                                    (0, 0)
                                }
                            };
                            if l != 0 || r != 0 {
                                d.set_speeds(l, r);
                                drop(d);
                                *dist_ctrl.lock().await = Some(DistanceCtrl {
                                    target_m: distance_mm / 1000.0,
                                    traveled_m: 0.0,
                                });
                                log::info!("[motor-bridge] move {} {}mm speed={}",
                                    direction, distance_mm as u32, speed);
                            }
                        }
                        _ => {}
                    }
                }
            }
            Event::Stop(cause) => {
                log::info!("[motor-bridge] Stop: {:?}, exiting", cause);
                break;
            }
            _ => {}
        }
    }

    driver.lock().unwrap().stop();
    log::info!("[motor-bridge] Shutdown");
    Ok(())
}
