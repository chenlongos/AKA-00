//! 机械臂服务 —— 接收上层控制命令，通过 dora 转发到 arm-bridge 节点执行
//!
//! 对应 `app/services/control_service.py` 的 arm 相关职责（_apply_arm_action
//! / update_arm_angles / preview_arm_angle / _do_grab / _do_release）。
//!
//! 关键设计：
//! - arm_angles.json 的所有权在 web-server（启动时 cwd = $DORA_HOME，文件就在
//!   cwd/arm_angles.json）。front-end 通过 /api/arm/angles 读写它。
//! - 高层动作 grab / release 由本服务读 arm_angles.json 后展开成 set_angle 序列
//!   再下发，避免 arm-bridge 也去读 JSON（职责单一）。
//! - 展开逻辑直接照搬 `src/arm_control/zl/zp10s/uart_control.py:62-90` 的
//!   Python grab() / release()，时序也一致（time.sleep → tokio::time::sleep）。

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use dora_node_api::DoraNode;
use serde::Serialize;
use tokio::sync::Mutex as TokioMutex;

/// 机械臂状态（与 arm-bridge 上报的 ArmStatus 字段名对齐）
#[derive(Debug, Clone, Serialize, Default)]
pub struct ArmStatus {
    pub angles: BTreeMap<u8, u16>,
    pub torque: String,
    pub last_action: String,
}

/// ZP10S 默认角度（与 `src/arm_control/angle_config.py:9-18` 完全一致）。
/// 文件丢失或字段缺失时回退到这里。
const DEFAULT_ZP10S_ANGLES: &[(&str, u16)] = &[
    ("servo0_prepare", 245),
    ("servo1_prepare", 180),
    ("servo2_prepare", 150),
    ("servo2_approach", 150),
    ("servo2_grab", 90),
    ("servo0_lift", 200),
    ("servo1_lift", 180),
    ("servo2_lift", 90),
];

pub struct ArmService {
    node: Arc<TokioMutex<DoraNode>>,
    status: Arc<Mutex<ArmStatus>>,
    angles_path: PathBuf,
    driver: String,
}

impl ArmService {
    pub fn new(node: Arc<TokioMutex<DoraNode>>, arm_cfg: dora_config::ArmConfig) -> Self {
        // 优先级：ARM_ANGLES_PATH 环境变量 > cwd/arm_angles.json
        let angles_path = std::env::var("ARM_ANGLES_PATH")
            .map(PathBuf::from)
            .unwrap_or_else(|_| PathBuf::from("arm_angles.json"));
        log::info!(
            "[arm] angles file = {} (override via ARM_ANGLES_PATH)",
            angles_path.display()
        );
        Self {
            node,
            status: Arc::new(Mutex::new(ArmStatus::default())),
            angles_path,
            driver: arm_cfg.backend,
        }
    }

    pub fn driver_name(&self) -> &str {
        &self.driver
    }

    pub fn status(&self) -> ArmStatus {
        self.status.lock().unwrap().clone()
    }

    /// 由 main.rs 的 dora 事件循环回调，更新 arm_status 缓存
    pub fn update_status(&self, new_status: ArmStatus) {
        *self.status.lock().unwrap() = new_status;
    }

    /// 读取 arm_angles.json（缺失字段回退到 DEFAULT_ZP10S_ANGLES）
    pub fn load_angles(&self) -> BTreeMap<String, u16> {
        let mut out: BTreeMap<String, u16> = DEFAULT_ZP10S_ANGLES
            .iter()
            .map(|(k, v)| (k.to_string(), *v))
            .collect();

        if let Ok(text) = std::fs::read_to_string(&self.angles_path) {
            // 支持两种格式：
            //   1. {"servo0_prepare": 245, ...}             （ZP10S 平铺格式，老格式）
            //   2. {"zp10s": {"servo0_prepare": 245, ...}} （按 driver 嵌套）
            // 与 `src/arm_control/angle_config.py::load_arm_angles` 行为对齐。
            if let Ok(v) = serde_json::from_slice::<serde_json::Value>(text.as_bytes()) {
                let source = v
                    .get(&self.driver)
                    .and_then(|x| x.as_object())
                    .or_else(|| v.as_object());

                if let Some(obj) = source {
                    for (k, val) in obj {
                        if let Some(n) = val.as_u64() {
                            if let Some(default) = DEFAULT_ZP10S_ANGLES
                                .iter()
                                .find(|(name, _)| *name == k.as_str())
                            {
                                out.insert(default.0.to_string(), n as u16);
                            }
                        }
                    }
                }
            }
        }

        out
    }

    /// 保存 arm_angles.json（保持现有格式：ZP10S 走平铺，其他 driver 走嵌套）
    pub fn save_angles(
        &self,
        driver: &str,
        angles: &BTreeMap<String, u16>,
    ) -> Result<(), String> {
        if driver != self.driver {
            return Err(format!(
                "driver mismatch: expected {}, got {}",
                self.driver, driver
            ));
        }

        // 仅保留 DEFAULT_ZP10S_ANGLES 里出现的 key（防前端发任意字段）
        let normalized: BTreeMap<String, u16> = DEFAULT_ZP10S_ANGLES
            .iter()
            .filter_map(|(k, default)| {
                let v = angles.get(*k).copied().unwrap_or(*default);
                Some((k.to_string(), v))
            })
            .collect();

        let to_write = if driver == "zp10s" {
            // ZP10S 沿用平铺格式以兼容现有文件
            serde_json::Value::Object(
                normalized
                    .iter()
                    .map(|(k, v)| (k.clone(), serde_json::json!(v)))
                    .collect(),
            )
        } else {
            serde_json::json!({ driver: normalized })
        };

        let body = serde_json::to_string_pretty(&to_write)
            .map_err(|e| format!("serialize: {}", e))?;
        std::fs::write(&self.angles_path, body + "\n")
            .map_err(|e| format!("write {}: {}", self.angles_path.display(), e))?;
        Ok(())
    }

    /// 处理来自 WebSocket / HTTP 的 JSON 命令。
    /// grab / release 在这里展开成 set_angle 序列再下发。
    pub async fn handle_json_cmd(&self, cmd: &serde_json::Value) {
        let command = cmd.get("command").and_then(|v| v.as_str()).unwrap_or("");
        match command {
            "grab" => {
                if let Err(e) = self.run_grab().await {
                    log::warn!("[arm] grab failed: {}", e);
                }
            }
            "release" => {
                if let Err(e) = self.run_release().await {
                    log::warn!("[arm] release failed: {}", e);
                }
            }
            _ => {
                // 细粒度原语直接转发
                self.send(cmd).await;
            }
        }
    }

    /// 立即下发单舵机指令
    pub async fn set_angle(&self, servo_id: u8, angle: u16) {
        let payload = serde_json::json!({
            "command": "set_angle",
            "servo_id": servo_id,
            "angle": angle,
        });
        self.send(&payload).await;
    }

    pub async fn torque(&self, on: bool) {
        let payload = serde_json::json!({
            "command": "torque",
            "on": on,
        });
        self.send(&payload).await;
    }

    pub async fn stop(&self) {
        let payload = serde_json::json!({ "command": "stop" });
        self.send(&payload).await;
    }

    // ── grab / release 编排（照搬 uart_control.py:62-90）──

    async fn run_grab(&self) -> Result<(), String> {
        let angles = self.load_angles();
        let get = |k: &str| -> u16 {
            angles
                .get(k)
                .copied()
                .or_else(|| {
                    DEFAULT_ZP10S_ANGLES
                        .iter()
                        .find(|(name, _)| *name == k)
                        .map(|(_, v)| *v)
                })
                .unwrap_or(150)
        };
        // id2_angle_open / id2_angle_close 在 Python 里就是 servo2_prepare / servo2_grab
        let open = get("servo2_prepare");
        let close = get("servo2_grab");

        // 1. 张开夹爪
        self.set_angle(2, open).await;
        tokio::time::sleep(Duration::from_millis(500)).await;

        // 2. 准备位
        self.set_angle(0, get("servo0_prepare")).await;
        self.set_angle(1, get("servo1_prepare")).await;
        self.set_angle(2, get("servo2_approach")).await;
        tokio::time::sleep(Duration::from_millis(1000)).await;

        // 3. 闭合夹爪
        self.set_angle(2, close).await;
        tokio::time::sleep(Duration::from_millis(2000)).await;

        // 4. 抬起
        self.set_angle(0, get("servo0_lift")).await;
        self.set_angle(1, get("servo1_lift")).await;
        self.set_angle(2, get("servo2_lift")).await;

        log::info!("[arm] grab sequence done");
        Ok(())
    }

    async fn run_release(&self) -> Result<(), String> {
        let open = self
            .load_angles()
            .get("servo2_prepare")
            .copied()
            .unwrap_or(150);
        self.set_angle(2, open).await;
        log::info!("[arm] release sequence done");
        Ok(())
    }

    async fn send(&self, payload: &serde_json::Value) {
        let bytes = serde_json::to_vec(payload).unwrap_or_default();
        let mut node = self.node.lock().await;
        let _ = node.send_output_bytes(
            "arm_cmd".into(),
            BTreeMap::new(),
            bytes.len(),
            &bytes,
        );
    }
}