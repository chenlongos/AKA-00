//! 机械臂服务 —— 接收上层控制命令，通过 dora 转发到 arm-bridge 节点执行
//!
//! 对应 `app/services/control_service.py` 的 arm 相关职责（_apply_arm_action
//! / update_arm_angles / preview_arm_angle / _do_grab / _do_release）。
//!
//! 角度配置结构（语义化，v0.5.2+）:
//!   {
//!     "grab_position": {"servo0": 245, "servo1": 180},
//!     "lift_position": {"servo0": 200, "servo1": 180},
//!     "gripper_open": 150,
//!     "gripper_close": 90
//!   }
//!
//! 与 `src/arm_control/angle_config.py` 完全对齐。
//!
//! 关键设计：
//! - arm_angles.json 的所有权在 web-server。文件位置：
//!   - 优先级：$ARM_ANGLES_PATH 环境变量（dev.sh / init.sh 显式设） > cwd/arm_angles.json
//!   - 真源是 dora/arm_angles.json（本地开发）或 build_release.sh 拷到 $DORA_HOME/arm_angles.json（板子）。
//! - front-end 通过 /api/arm/angles 读写同一路径，所以 dev/init 看到的所有改动落回真源。
//! - 高层动作 grab / release 由本服务读 arm_angles.json 后展开成 set_angle 序列
//!   再下发，避免 arm-bridge 也去读 JSON（职责单一）。

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

use dora_node_api::DoraNode;
use serde::{Deserialize, Serialize};
use tokio::sync::Mutex as TokioMutex;

use super::dora_send;

/// 机械臂状态（与 arm-bridge 上报的 ArmStatus 字段名对齐）
#[derive(Debug, Clone, Serialize, Default)]
pub struct ArmStatus {
    pub angles: BTreeMap<u8, u16>,
    pub torque: String,
    pub last_action: String,
}

// ── 语义化角度配置结构 ──

/// 一组舵机位置（如 grab_position / lift_position）
pub type ServoGroup = BTreeMap<String, u16>;

/// 语义化角度配置（新格式，v0.5.2+）
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct SemanticAngles {
    pub grab_position: ServoGroup,
    pub lift_position: ServoGroup,
    pub gripper_open: u16,
    pub gripper_close: u16,
}

/// ZP10S 默认角度（与 `src/arm_control/angle_config.py:DEFAULT_ZP10S_ARM_ANGLES` 一致）
const DEFAULT_ZP10S_ANGLES: SemanticAngles = SemanticAngles {
    grab_position: ServoGroup::new(),
    lift_position: ServoGroup::new(),
    gripper_open: 150,
    gripper_close: 90,
};

/// STS3215 默认角度（与 `src/arm_control/angle_config.py:DEFAULT_STS3215_ARM_ANGLES` 一致）
const DEFAULT_STS3215_ANGLES: SemanticAngles = SemanticAngles {
    grab_position: ServoGroup::new(),
    lift_position: ServoGroup::new(),
    gripper_open: 4000,
    gripper_close: 3000,
};

/// ZP10S 的 grab_position 默认 servo 值
const ZP10S_GRAB_DEFAULTS: &[(&str, u16)] = &[
    ("servo0", 245),
    ("servo1", 180),
];

/// ZP10S 的 lift_position 默认 servo 值
const ZP10S_LIFT_DEFAULTS: &[(&str, u16)] = &[
    ("servo0", 200),
    ("servo1", 180),
];

/// STS3215 的 grab_position 默认 servo 值
const STS3215_GRAB_DEFAULTS: &[(&str, u16)] = &[
    ("servo1", 1850),
    ("servo2", 2650),
];

/// STS3215 的 lift_position 默认 servo 值
const STS3215_LIFT_DEFAULTS: &[(&str, u16)] = &[
    ("servo1", 2300),
    ("servo2", 2100),
];

/// 夹爪舵机 ID：ZP10S 用 servo2，STS3215 用 servo3
const ZP10S_GRIPPER_SERVO: u8 = 2;
const STS3215_GRIPPER_SERVO: u8 = 3;

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

    /// 夹爪舵机 ID：ZP10S=2, STS3215=3
    pub fn gripper_servo_id(&self) -> u8 {
        if self.driver == "sts3215" { STS3215_GRIPPER_SERVO } else { ZP10S_GRIPPER_SERVO }
    }

    // ── 角度配置读写 ──

    /// 读取 arm_angles.json，返回语义化结构（自动迁移旧格式）
    pub fn load_angles(&self) -> SemanticAngles {
        let defaults = self.default_angles();
        let raw_data = self.read_angles_file();

        // 提取对应 driver 的数据
        let source = raw_data
            .get(&self.driver)
            .or_else(|| if self.driver == "zp10s" { Some(&raw_data) } else { None });

        // 尝试直接反序列化为 SemanticAngles（新格式）
        if let Some(src) = source {
            if let Ok(angles) = serde_json::from_value::<SemanticAngles>(src.clone()) {
                // 检查是否为旧格式（包含旧 key 名）
                let source_map: BTreeMap<String, serde_json::Value> = src
                    .as_object()
                    .map(|obj| obj.iter().map(|(k, v)| (k.clone(), v.clone())).collect())
                    .unwrap_or_default();

                if self.is_old_format(&source_map) {
                    let migrated = self.migrate_old_format(&source_map);
                    self.write_angles(&migrated);
                    return migrated;
                }

                // 新格式：用默认值填充缺失字段
                return SemanticAngles {
                    grab_position: {
                        let mut m = defaults.grab_position.clone();
                        for (k, v) in &angles.grab_position { m.insert(k.clone(), *v); }
                        m
                    },
                    lift_position: {
                        let mut m = defaults.lift_position.clone();
                        for (k, v) in &angles.lift_position { m.insert(k.clone(), *v); }
                        m
                    },
                    gripper_open: if angles.gripper_open > 0 { angles.gripper_open } else { defaults.gripper_open },
                    gripper_close: if angles.gripper_close > 0 { angles.gripper_close } else { defaults.gripper_close },
                };
            }
        }

        // 文件为空或格式不对，返回默认值
        defaults
    }

    /// 保存 arm_angles.json（语义化格式，接受 SemanticAngles 结构体）
    pub fn save_angles_semantic(
        &self,
        driver: &str,
        angles: &SemanticAngles,
    ) -> Result<SemanticAngles, String> {
        if driver != self.driver {
            return Err(format!(
                "driver mismatch: expected {}, got {}",
                self.driver, driver
            ));
        }

        let defaults = self.default_angles();
        let normalized = SemanticAngles {
            grab_position: {
                let mut m = defaults.grab_position.clone();
                for (k, v) in &angles.grab_position { m.insert(k.clone(), *v); }
                m
            },
            lift_position: {
                let mut m = defaults.lift_position.clone();
                for (k, v) in &angles.lift_position { m.insert(k.clone(), *v); }
                m
            },
            gripper_open: angles.gripper_open,
            gripper_close: angles.gripper_close,
        };

        // 检查文件是否为多 driver 格式
        let raw_data = self.read_angles_file();
        let is_multi = raw_data.as_object()
            .map(|obj| obj.keys().any(|k| k == "zp10s" || k == "sts3215"))
            .unwrap_or(false);

        if self.driver == "zp10s" && !is_multi {
            self.write_angles(&normalized);
        } else {
            let mut data: serde_json::Map<String, serde_json::Value> = if is_multi {
                raw_data.as_object().cloned().unwrap_or_default()
            } else {
                serde_json::Map::new()
            };
            data.insert(
                self.driver.clone(),
                serde_json::to_value(&normalized).unwrap_or_default(),
            );
            let body = serde_json::to_string_pretty(&data)
                .map_err(|e| format!("serialize: {}", e))?;
            std::fs::write(&self.angles_path, body + "\n")
                .map_err(|e| format!("write {}: {}", self.angles_path.display(), e))?;
        }
        Ok(normalized)
    }

    /// 保存原始 SemanticAngles 到文件（路由 preview 用）
    pub fn save_angles_semantic_raw(
        &self,
        driver: &str,
        angles: &SemanticAngles,
    ) -> Result<(), String> {
        self.save_angles_semantic(driver, angles)?;
        Ok(())
    }

    // ── 语义化访问函数 ──

    /// 读取 grab_position 中某个 servo 的角度
    pub fn get_grab_servo(&self, servo_key: &str) -> u16 {
        let angles = self.load_angles();
        angles.grab_position.get(servo_key).copied().unwrap_or_else(|| {
            self.default_grab_servo(servo_key)
        })
    }

    /// 读取 lift_position 中某个 servo 的角度
    pub fn get_lift_servo(&self, servo_key: &str) -> u16 {
        let angles = self.load_angles();
        angles.lift_position.get(servo_key).copied().unwrap_or_else(|| {
            self.default_lift_servo(servo_key)
        })
    }

    /// 读取 gripper_open
    pub fn get_gripper_open(&self) -> u16 {
        self.load_angles().gripper_open
    }

    /// 读取 gripper_close
    pub fn get_gripper_close(&self) -> u16 {
        self.load_angles().gripper_close
    }

    // ── 命令处理 ──

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
                let _ = self.send(cmd).await;
            }
        }
    }

    /// 立即下发单舵机指令
    pub async fn set_angle(&self, servo_id: u8, angle: u16) -> Result<(), String> {
        let payload = serde_json::json!({
            "command": "set_angle",
            "servo_id": servo_id,
            "angle": angle,
        });
        self.send(&payload).await
    }

    pub async fn torque(&self, on: bool) {
        let payload = serde_json::json!({
            "command": "torque",
            "on": on,
        });
        let _ = self.send(&payload).await;
    }

    pub async fn stop(&self) {
        let payload = serde_json::json!({ "command": "stop" });
        let _ = self.send(&payload).await;
    }

    // ── grab / release 编排（语义化角度，与 Python 对齐）──

    async fn run_grab(&self) -> Result<(), String> {
        let angles = self.load_angles();
        let gripper_servo = self.gripper_servo_id();
        let open = angles.gripper_open;
        let close = angles.gripper_close;

        // 1. 张开夹爪
        self.set_angle(gripper_servo, open).await?;
        tokio::time::sleep(std::time::Duration::from_millis(500)).await;

        // 2. 准备位（grab_position 中各舵机 + 夹爪接近）
        for (sk, &angle) in &angles.grab_position {
            if let Some(id) = servo_key_to_id(sk) {
                self.set_angle(id, angle).await?;
            }
        }
        // 夹爪接近位 = 当前张开角度（保持张开等待抓取）
        tokio::time::sleep(std::time::Duration::from_millis(1000)).await;

        // 3. 闭合夹爪
        self.set_angle(gripper_servo, close).await?;
        tokio::time::sleep(std::time::Duration::from_millis(2000)).await;

        // 4. 抬起（lift_position 中各舵机 + 保持夹爪闭合）
        for (sk, &angle) in &angles.lift_position {
            if let Some(id) = servo_key_to_id(sk) {
                self.set_angle(id, angle).await?;
            }
        }

        log::info!("[arm] grab sequence done");
        Ok(())
    }

    async fn run_release(&self) -> Result<(), String> {
        let open = self.get_gripper_open();
        let gripper_servo = self.gripper_servo_id();
        self.set_angle(gripper_servo, open).await?;
        log::info!("[arm] release sequence done");
        Ok(())
    }

    async fn send(&self, payload: &serde_json::Value) -> Result<(), String> {
        let bytes = serde_json::to_vec(payload).unwrap_or_default();
        dora_send::send_output(&self.node, "arm_cmd", &bytes)
            .await
            .map_err(|e| format!("arm send failed: {e}"))
    }

    // ── 内部辅助函数 ──

    fn default_angles(&self) -> SemanticAngles {
        if self.driver == "sts3215" {
            let mut a = DEFAULT_STS3215_ANGLES.clone();
            a.grab_position = STS3215_GRAB_DEFAULTS.iter().map(|(k, v)| (k.to_string(), *v)).collect();
            a.lift_position = STS3215_LIFT_DEFAULTS.iter().map(|(k, v)| (k.to_string(), *v)).collect();
            a
        } else {
            let mut a = DEFAULT_ZP10S_ANGLES.clone();
            a.grab_position = ZP10S_GRAB_DEFAULTS.iter().map(|(k, v)| (k.to_string(), *v)).collect();
            a.lift_position = ZP10S_LIFT_DEFAULTS.iter().map(|(k, v)| (k.to_string(), *v)).collect();
            a
        }
    }

    fn default_grab_servo(&self, servo_key: &str) -> u16 {
        let defaults = if self.driver == "sts3215" { STS3215_GRAB_DEFAULTS } else { ZP10S_GRAB_DEFAULTS };
        defaults.iter().find(|(k, _)| *k == servo_key).map(|(_, v)| *v).unwrap_or(0)
    }

    fn default_lift_servo(&self, servo_key: &str) -> u16 {
        let defaults = if self.driver == "sts3215" { STS3215_LIFT_DEFAULTS } else { ZP10S_LIFT_DEFAULTS };
        defaults.iter().find(|(k, _)| *k == servo_key).map(|(_, v)| *v).unwrap_or(0)
    }

    fn read_angles_file(&self) -> serde_json::Value {
        std::fs::read_to_string(&self.angles_path)
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or(serde_json::Value::Object(Default::default()))
    }

    fn write_angles(&self, angles: &SemanticAngles) {
        let to_write = if self.driver == "zp10s" {
            // ZP10S 平铺格式
            serde_json::to_value(angles).unwrap_or_default()
        } else {
            serde_json::json!({ self.driver.clone(): angles })
        };
        let body = serde_json::to_string_pretty(&to_write).unwrap_or_default();
        let _ = std::fs::write(&self.angles_path, body + "\n");
    }

    /// 检测旧格式：存在 servoX_prepare / servoX_lift / servoX_grab / servoX_approach / servoX_enter 键
    fn is_old_format(&self, data: &BTreeMap<String, serde_json::Value>) -> bool {
        data.keys().any(|k| {
            k.starts_with("servo")
                && (k.contains("_prepare")
                    || k.contains("_lift")
                    || k.contains("_grab")
                    || k.contains("_approach")
                    || k.contains("_enter"))
        })
    }

    /// 将旧格式迁移到新语义化格式（与 Python angle_config.py:_migrate_old_format 对齐）
    fn migrate_old_format(&self, data: &BTreeMap<String, serde_json::Value>) -> SemanticAngles {
        let get = |key: &str, default: u16| -> u16 {
            data.get(key)
                .and_then(|v| v.as_u64())
                .map(|n| n as u16)
                .unwrap_or(default)
        };

        if self.driver == "sts3215" {
            let mut grab = ServoGroup::new();
            grab.insert("servo1".into(), get("servo1_enter", 1850));
            grab.insert("servo2".into(), get("servo2_enter", 2650));

            let mut lift = ServoGroup::new();
            lift.insert("servo1".into(), get("servo1_lift", 2300));
            lift.insert("servo2".into(), get("servo2_lift", 2100));

            SemanticAngles {
                grab_position: grab,
                lift_position: lift,
                gripper_open: get("servo3_prepare", 4000),
                gripper_close: get("servo3_grab", 3000),
            }
        } else {
            // ZP10S
            let mut grab = ServoGroup::new();
            grab.insert("servo0".into(), get("servo0_prepare", 245));
            grab.insert("servo1".into(), get("servo1_prepare", 180));

            let mut lift = ServoGroup::new();
            lift.insert("servo0".into(), get("servo0_lift", 200));
            lift.insert("servo1".into(), get("servo1_lift", 180));

            SemanticAngles {
                grab_position: grab,
                lift_position: lift,
                gripper_open: get("servo2_prepare", 150),
                gripper_close: get("servo2_grab", 90),
            }
        }
    }
}

// ── 公共工具函数 ──

/// "servo0" → 0, "servo1" → 1, "servo2" → 2, ...
pub fn servo_key_to_id(key: &str) -> Option<u8> {
    key.strip_prefix("servo")?.chars().next()?.to_digit(10).map(|n| n as u8)
}

/// 解析前端 preview key，支持新旧两种格式：
///   - 旧格式: "servo0_prepare" → (0, old_key)
///   - 新格式: "grab_position.servo0" → (0, new_group)
///   - 新格式: "gripper_open" → 返回 None（由调用方根据 driver 决定夹爪 servo ID）
///   - 新格式: "gripper_close" → 同上
pub fn parse_arm_key(key: &str) -> Option<u8> {
    // gripper_open / gripper_close 无固定 servo ID，调用方处理
    if key == "gripper_open" || key == "gripper_close" {
        return None;
    }
    // 新格式: "xxx.servoN"
    if let Some(idx) = key.rfind(".servo") {
        let after_dot = &key[idx + 1..]; // "servoN"
        return servo_key_to_id(after_dot);
    }
    // 旧格式: "servoN_..."
    if let Some(rest) = key.strip_prefix("servo") {
        if let Some(c) = rest.chars().next() {
            if let Some(n) = c.to_digit(10) {
                return Some(n as u8);
            }
        }
    }
    None
}
