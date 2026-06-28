//! dora state-node — 统一机器人状态管理
//!
//! 对应 `src/state/__init__.py` 的 StateCollector。
//! 聚合所有子系统的状态，每 200ms 发布一次 robot_state。
//!
//! 输入:  motor_status, camera_status, arm_status (future)
//! 输出:  robot_state (JSON)

use std::collections::BTreeMap;
use std::sync::Mutex;

use arrow::array::AsArray;
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use serde::Serialize;

// ── 统一状态 ──

#[derive(Debug, Clone, Serialize, Default)]
struct RobotState {
    // 电机
    motor_left_rpm: i32,
    motor_right_rpm: i32,
    // 摄像头
    camera_on: bool,
    camera_fps: u32,
    // 机械臂 (future)
    arm_angles: Vec<i32>,
    // 系统
    uptime_secs: u64,
}

struct StateStore {
    state: Mutex<RobotState>,
    started: std::time::Instant,
}

impl StateStore {
    fn new() -> Self {
        Self { state: Mutex::new(RobotState::default()), started: std::time::Instant::now() }
    }

    fn update<T: FnOnce(&mut RobotState)>(&self, f: T) {
        let mut s = self.state.lock().unwrap();
        f(&mut s);
    }

    fn snapshot(&self) -> RobotState {
        let mut s = self.state.lock().unwrap().clone();
        s.uptime_secs = self.started.elapsed().as_secs();
        s
    }
}

fn main() -> Result<()> {
    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(run()).unwrap_or_else(|e| eprintln!("state-node error: {:?}", e));
    Ok(())
}

async fn run() -> Result<()> {
    let (node, mut events) = DoraNode::init_from_env()
        .wrap_err("Failed to init dora node")?;
    println!("[state] Dora node initialized");

    let store = std::sync::Arc::new(StateStore::new());
    let node = std::sync::Arc::new(tokio::sync::Mutex::new(node));

    // 每 200ms 发布聚合状态
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

    // 接收各子系统状态更新
    while let Some(event) = events.next().await {
        if let Event::Input { id, data, .. } = event {
            let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
            let bytes = uint8_arr.values();

            match id.to_string().as_str() {
                "motor_status" => {
                    if let Ok(v) = serde_json::from_slice::<serde_json::Value>(bytes) {
                        store.update(|s| {
                            s.motor_left_rpm = v["left_rpm"].as_i64().unwrap_or(0) as i32;
                            s.motor_right_rpm = v["right_rpm"].as_i64().unwrap_or(0) as i32;
                        });
                    }
                }
                "camera_status" => {
                    if let Ok(v) = serde_json::from_slice::<serde_json::Value>(bytes) {
                        store.update(|s| {
                            s.camera_on = v["camera_on"].as_bool().unwrap_or(false);
                            if let Some(fps) = v["fps"].as_u64() {
                                s.camera_fps = fps as u32;
                            }
                        });
                    }
                }
                _ => {}
            }
        } else if let Event::Stop(cause) = event {
            println!("[state] Stop: {:?}, exiting", cause);
            break;
        }
    }

    println!("[state] Shutdown");
    Ok(())
}
