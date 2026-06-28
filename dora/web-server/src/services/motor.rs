//! 电机控制服务 —— 接收上层控制命令，通过 dora 转发到 motor-bridge 节点执行
//!
//! 对应 `app/services/control_service.py` 的职责。

use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};

use dora_node_api::DoraNode;
use serde::Serialize;
use tokio::sync::Mutex as TokioMutex;

/// 电机实时状态（对应 Python StateCollector 的 get_status()）
#[derive(Debug, Clone, Serialize)]
pub struct MotorStatus {
    pub left_rpm: i32,    // 左轮 RPM
    pub right_rpm: i32,   // 右轮 RPM
}

/// 控制动作
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Action {
    Up,
    Down,
    Left,
    Right,
    Stop,
    Grab,
    Release,
}

pub struct MotorService {
    node: Arc<TokioMutex<DoraNode>>,
    status: Arc<Mutex<MotorStatus>>,
}

impl MotorService {
    pub fn new(node: Arc<TokioMutex<DoraNode>>) -> Self {
        Self {
            node,
            status: Arc::new(Mutex::new(MotorStatus {
                left_rpm: 0,
                right_rpm: 0,
            })),
        }
    }

    pub fn status(&self) -> MotorStatus {
        self.status.lock().unwrap().clone()
    }

    /// 方向控制（/api/control?action=up|down|left|right|stop）
    pub async fn action(&self, action: Action, speed: u8, duration_ms: u32) {
        let json = serde_json::json!({
            "command": "action",
            "action": action,
            "speed": speed,
            "duration": duration_ms
        });
        self.send_control(&json).await;
    }

    /// 直接设置电机速度（/api/motor/direct?left=N&right=N&duration=N）
    pub async fn direct(&self, left: i32, right: i32, duration_ms: u32) {
        let json = serde_json::json!({
            "command": "direct",
            "left": left,
            "right": right,
            "duration": duration_ms
        });
        self.send_control(&json).await;
    }

    /// 处理来自 WebSocket 的 JSON 命令
    pub async fn handle_json_cmd(&self, cmd: &serde_json::Value) {
        self.send_control(cmd).await;
    }

    /// 处理来自 WebSocket 的 joystick 输入（x, y: -127..127）
    pub async fn joystick(&self, x: i8, y: i8) {
        let json = serde_json::json!({
            "command": "joystick",
            "x": x,
            "y": y
        });
        self.send_control(&json).await;
    }

    /// 从 motor-bridge 回报更新 RPM
    pub fn update_rpm(&self, left: i32, right: i32) {
        let mut s = self.status.lock().unwrap();
        s.left_rpm = left;
        s.right_rpm = right;
    }

    async fn send_control(&self, data: &serde_json::Value) {
        let bytes = serde_json::to_vec(data).unwrap_or_default();
        let mut node = self.node.lock().await;
        let _ = node.send_output_bytes(
            "motor_cmd".into(),
            BTreeMap::new(),
            bytes.len(),
            &bytes,
        );
    }
}
