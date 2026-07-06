//! dora state-node — 统一机器人状态管理
//!
//! 对应 `src/state/__init__.py` 的 `StateCollector` / `RobotStatus`。
//! 聚合所有子系统的状态，每 200ms 发布一次 robot_state。
//!
//! 输入:  motor_status (来自 motor-bridge)
//! 输出:  robot_state (JSON) — 字段名 / 类型与 Python `RobotStatus` 一致

use std::collections::BTreeMap;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

use arrow::array::AsArray;
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use serde::Serialize;

// ── 统一状态（与 Python `RobotStatus` 一一对应）──

#[derive(Debug, Clone, Serialize)]
struct RobotState {
    /// 左轮线速度 (m/s)
    left_speed: f32,
    /// 右轮线速度 (m/s)
    right_speed: f32,
    /// 左轮目标速度（由 motor_cmd 解析得到；当前未连入 input，置 0）
    left_target: f32,
    /// 右轮目标速度
    right_target: f32,
    /// 夹爪状态: "open" / "closed" / "moving" / "unknown"
    gripper_status: String,
    /// 夹爪目标: 0=释放, 1=夹取（zp10s 暂未对接，置 0）
    gripper_target: i32,
    /// 机械臂每个舵机最近一次 set_angle 的角度（ID → 角度）
    arm_angles: BTreeMap<u8, u16>,
    /// 机械臂扭力状态: "on" / "off" / "unknown"
    arm_torque: String,
    /// 机械臂最近一次动作（前端调试用，如 "set_angle 0 150" / "grab" / "release"）
    arm_last_action: String,
    /// unix 时间戳毫秒（对齐 Python `int(time.time() * 1000)`）
    timestamp_ms: i64,
}

impl Default for RobotState {
    fn default() -> Self {
        Self {
            left_speed: 0.0,
            right_speed: 0.0,
            left_target: 0.0,
            right_target: 0.0,
            gripper_status: "unknown".into(),
            gripper_target: 0,
            arm_angles: BTreeMap::new(),
            arm_torque: "unknown".into(),
            arm_last_action: String::new(),
            timestamp_ms: 0,
        }
    }
}

struct StateStore {
    state: Mutex<RobotState>,
    chassis: dora_config::ChassisConfig,
}

impl StateStore {
    fn new(chassis: dora_config::ChassisConfig) -> Self {
        Self { state: Mutex::new(RobotState::default()), chassis }
    }

    fn update<T: FnOnce(&mut RobotState)>(&self, f: T) {
        let mut s = self.state.lock().unwrap();
        f(&mut s);
    }

    /// 取快照并刷上当前时间戳。
    fn snapshot(&self) -> RobotState {
        let mut s = self.state.lock().unwrap().clone();
        s.timestamp_ms = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_millis() as i64)
            .unwrap_or(0);
        s
    }

    /// 把 motor-bridge 上报的 motor_rpm 换算成 m/s 再存。
    fn update_motor_rpm(&self, left_motor_rpm: i32, right_motor_rpm: i32) {
        let left_mps = rpm_to_mps(left_motor_rpm, &self.chassis);
        let right_mps = rpm_to_mps(right_motor_rpm, &self.chassis);
        self.update(|s| {
            s.left_speed = left_mps;
            s.right_speed = right_mps;
        });
    }

    /// 更新目标速度（m/s）。来自 web-server 的 motor_cmd 解析。
    fn update_target(&self, left_target: f32, right_target: f32) {
        self.update(|s| {
            s.left_target = left_target;
            s.right_target = right_target;
        });
    }
}

/// motor_rpm → m/s，与 web-server 端 `MotorService::rpm_to_mps` 公式一致：
///   wheel_rpm = motor_rpm / gear_ratio
///   linear_m_per_s = wheel_rpm × π × (wheel_diameter_mm/1000) / 60
fn rpm_to_mps(motor_rpm: i32, chassis: &dora_config::ChassisConfig) -> f32 {
    let wheel_rpm = (motor_rpm as f32) / (chassis.gear_ratio as f32);
    let diameter_m = (chassis.wheel_diameter_mm as f32) / 1000.0;
    wheel_rpm * std::f32::consts::PI * diameter_m / 60.0
}

/// 解析 web-server 发来的 motor_cmd JSON，得到 (left_target, right_target)。
/// 与 Python `set_target_speed` 一致：存原始 PWM% (-100..100)，不是 m/s。
///   - `action`     up=(+s,+s), down=(-s,-s), left=(-s,+s), right=(+s,-s), stop=(0,0)
///   - `direct`     取 payload 的 left/right
///   - `joystick`   与 motor-bridge `joystick_to_tank` 公式一致：i8 /127 ×100 夹到 ±100
/// `grab` / `release` 是夹爪命令，不影响 wheel target，返回 None。
fn parse_motor_cmd_target(bytes: &[u8]) -> Option<(f32, f32)> {
    let v: serde_json::Value = serde_json::from_slice(bytes).ok()?;
    let command = v.get("command")?.as_str()?;

    let pair: (f32, f32) = match command {
        "action" => {
            let action = v.get("action")?.as_str()?;
            let speed = v.get("speed")?.as_f64()? as f32;
            match action {
                "up" => (speed, speed),
                "down" => (-speed, -speed),
                "left" => (-speed, speed),
                "right" => (speed, -speed),
                "stop" => (0.0, 0.0),
                "grab" | "release" => return None, // 夹爪指令
                _ => return None,
            }
        }
        "direct" => {
            let l = v.get("left")?.as_f64()? as f32;
            let r = v.get("right")?.as_f64()? as f32;
            (l, r)
        }
        "joystick" => {
            // 与 motor-bridge `joystick_to_tank` 完全一致
            let x = v.get("x").and_then(|x| x.as_i64()).unwrap_or(0) as i8 as f32;
            let y = v.get("y").and_then(|x| x.as_i64()).unwrap_or(0) as i8 as f32;
            let fy = y / 127.0;
            let fx = x / 127.0;
            let c = |v: f32| (v * 100.0).clamp(-100.0, 100.0);
            (c(fy + fx), c(fy - fx))
        }
        _ => return None,
    };

    Some(pair)
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
        .unwrap_or_else(|e| log::error!("state-node error: {:?}", e));
    Ok(())
}

async fn run() -> Result<()> {
    let config = dora_config::Config::load();

    let (node, mut events) = DoraNode::init_from_env()
        .wrap_err("Failed to init dora node")?;
    log::info!("[state] Dora node initialized");

    let store = std::sync::Arc::new(StateStore::new(config.chassis.clone()));
    let node = std::sync::Arc::new(tokio::sync::Mutex::new(node));

    // 每 200ms 发布聚合状态（带时间戳）
    let store_pub = store.clone();
    let node_pub = node.clone();
    tokio::spawn(async move {
        let mut tick = tokio::time::interval(std::time::Duration::from_millis(200));
        loop {
            tick.tick().await;
            let snapshot = store_pub.snapshot();
            let json = serde_json::to_vec(&snapshot).unwrap_or_default();
            let mut n = node_pub.lock().await;
            let _ = n.send_output_bytes(
                "robot_state".into(),
                BTreeMap::new(),
                json.len(),
                &json,
            );
        }
    });

    // 接收 motor-bridge 的 motor_status + web-server 的 motor_cmd
    while let Some(event) = events.next().await {
        if let Event::Input { id, data, .. } = event {
            let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
            let bytes = uint8_arr.values();

            match id.to_string().as_str() {
                "motor_status" => {
                    if let Ok(v) = serde_json::from_slice::<serde_json::Value>(bytes) {
                        let left = v["left_rpm"].as_i64().unwrap_or(0) as i32;
                        let right = v["right_rpm"].as_i64().unwrap_or(0) as i32;
                        store.update_motor_rpm(left, right);
                    }
                }
                "motor_cmd" => {
                    // 解析 action/direct/joystick 算出 left_target/right_target (PWM%)
                    if let Some((l, r)) = parse_motor_cmd_target(bytes) {
                        store.update_target(l, r);
                    }
                    // grab / release / 解析失败：不影响 wheel target
                }
                "arm_status" => {
                    // arm-bridge 每次命令后立刻 publish：
                    //   { "angles": {"0":150,"1":180,...}, "torque":"on|off|unknown",
                    //     "last_action":"set_angle 0 150" }
                    if let Ok(v) = serde_json::from_slice::<serde_json::Value>(bytes) {
                        let mut angles = BTreeMap::new();
                        if let Some(obj) = v.get("angles").and_then(|x| x.as_object()) {
                            for (k, val) in obj {
                                if let (Ok(id), Some(a)) =
                                    (k.parse::<u8>(), val.as_u64())
                                {
                                    angles.insert(id, a as u16);
                                }
                            }
                        }
                        let torque = v
                            .get("torque")
                            .and_then(|x| x.as_str())
                            .unwrap_or("unknown")
                            .to_string();
                        let last = v
                            .get("last_action")
                            .and_then(|x| x.as_str())
                            .unwrap_or("")
                            .to_string();
                        store.update(|s| {
                            s.arm_angles = angles;
                            s.arm_torque = torque;
                            s.arm_last_action = last;
                        });
                    }
                }
                "arm_cmd" => {
                    // web-server 在 expand grab/release 时，最初下发的 payload 顶层
                    // command 字段就是 "grab" / "release" / "set_angle" 等。
                    // 把高层动作记下来给前端调试用。
                    if let Ok(v) = serde_json::from_slice::<serde_json::Value>(bytes) {
                        if let Some(act) = v.get("command").and_then(|x| x.as_str()) {
                            if act == "grab" || act == "release" {
                                store.update(|s| {
                                    s.arm_last_action = act.to_string();
                                });
                            }
                        }
                    }
                }
                _ => {}
            }
        } else if let Event::Stop(cause) = event {
            log::info!("[state] Stop: {:?}, exiting", cause);
            break;
        }
    }

    log::info!("[state] Shutdown");
    Ok(())
}
