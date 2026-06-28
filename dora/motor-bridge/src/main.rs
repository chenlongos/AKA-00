//! dora motor-bridge — 接收 web-server 控制命令，驱动底盘电机
//!
//! 输入:  motor_cmd (JSON)     — 来自 web-server
//! 输出:  motor_status (JSON)  — 回报实时速度（待实现双向通信）
//!
//! 硬件层：driver/ 屏蔽底层差异
//!   dev::DevMotor   — 开发环境，打印命令到 stdout
//!   tt_pid::TtPidDriver — 真实硬件，UART 协议对接 ESP32-C3

mod driver;

use arrow::array::AsArray;
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use driver::{create_driver, DriverConfig};
use eyre::{Context, Result};
use serde::Deserialize;

// ── 命令解析（与 web-server services/motor.rs 对应）──

#[derive(Debug, Deserialize)]
#[serde(tag = "command")]
enum MotorCmd {
    #[serde(rename = "action")]
    Action { action: String, speed: u8, duration: u32 },
    #[serde(rename = "direct")]
    Direct { left: i32, right: i32, duration: u32 },
    #[serde(rename = "joystick")]
    Joystick { x: i8, y: i8 },
    #[serde(rename = "raw_command")]
    RawCommand { cmd: String },
}

/// 摇杆 → 坦克转向 (x,y: -127..127)
fn joystick_to_tank(x: i8, y: i8) -> (i32, i32) {
    let fy = (y as f32) / 127.0;
    let fx = (x as f32) / 127.0;
    let clamp = |v: f32| (v * 100.0).clamp(-100.0, 100.0) as i32;
    (clamp(fy + fx), clamp(fy - fx))
}

fn main() -> Result<()> {
    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(run()).unwrap_or_else(|e| eprintln!("motor-bridge error: {:?}", e));
    Ok(())
}

async fn run() -> Result<()> {
    let (_node, mut events) = DoraNode::init_from_env()
        .wrap_err("Failed to init dora node")?;
    println!("[motor-bridge] Dora node initialized");

    // 根据环境变量或配置选择驱动（默认 dev）
    let config = DriverConfig::default();
    let mut driver = create_driver(&config);
    println!("[motor-bridge] Driver: {:?} backend", config.backend.as_deref().unwrap_or("dev"));

    while let Some(event) = events.next().await {
        match event {
            Event::Input { id, data, .. } if id.to_string() == "motor_cmd" => {
                let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
                let cmd: MotorCmd = match serde_json::from_slice(uint8_arr.values()) {
                    Ok(c) => c,
                    Err(e) => {
                        eprintln!("[motor-bridge] parse error: {:?}", e);
                        continue;
                    }
                };

                match cmd {
                    MotorCmd::Action { action, speed, duration } => {
                        let (l, r) = match action.as_str() {
                            "up" => (speed as i32, speed as i32),
                            "down" => (-(speed as i32), -(speed as i32)),
                            "left" => (-(speed as i32), speed as i32),
                            "right" => (speed as i32, -(speed as i32)),
                            "stop" => (0, 0),
                            _ => {
                                eprintln!("[motor-bridge] unknown action: {}", action);
                                continue;
                            }
                        };
                        driver.set_speeds(l, r);
                        if duration > 0 {
                            tokio::time::sleep(std::time::Duration::from_millis(duration as u64)).await;
                            driver.stop();
                        }
                    }
                    MotorCmd::Direct { left, right, duration } => {
                        driver.set_speeds(left, right);
                        if duration > 0 {
                            tokio::time::sleep(std::time::Duration::from_millis(duration as u64)).await;
                            driver.stop();
                        }
                    }
                    MotorCmd::Joystick { x, y } => {
                        let (l, r) = joystick_to_tank(x, y);
                        driver.set_speeds(l, r);
                    }
                    MotorCmd::RawCommand { cmd } => {
                        println!("[motor-bridge] raw_command: {}", cmd);
                    }
                }
            }
            Event::Stop(cause) => {
                println!("[motor-bridge] Stop: {:?}, exiting", cause);
                break;
            }
            _ => {}
        }
    }

    driver.stop();
    println!("[motor-bridge] Shutdown");
    Ok(())
}
