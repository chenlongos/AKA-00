//! dora 统一配置 — 对应 app/config.py 的 HardwareConfig
//!
//! 所有节点通过 `dora_config::load()` 读取同一份 config.toml。

use serde::Deserialize;

#[derive(Debug, Clone, Deserialize)]
pub struct Config {
    pub camera: CameraConfig,
    pub motor: MotorConfig,
    pub arm: ArmConfig,
    pub web: WebConfig,
    pub chassis: ChassisConfig,
    pub logging: LoggingConfig,
}

#[derive(Debug, Clone, Deserialize)]
pub struct CameraConfig {
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub jpeg_quality: u8,
}

#[derive(Debug, Clone, Deserialize)]
pub struct MotorConfig {
    pub backend: String,
    pub port: String,
    pub baudrate: u32,
    pub ppr: i32,
}

#[derive(Debug, Clone, Deserialize)]
pub struct ArmConfig {
    pub backend: String,
    pub port: String,
    pub baudrate: u32,
}

#[derive(Debug, Clone, Deserialize)]
pub struct WebConfig {
    pub port: u16,
}

/// 日志配置 —— motor-bridge / web-server / state-node 启动时用其 level 作为
/// env_logger 的默认 filter。RUST_LOG 仍然可以覆盖。
#[derive(Debug, Clone, Deserialize)]
pub struct LoggingConfig {
    pub level: String,
}

/// 底盘物理参数 —— 用于把电机 RPM 换算成线速度 m/s：
///   wheel_rpm       = motor_rpm / gear_ratio
///   linear_speed_ms = wheel_rpm × π × (wheel_diameter_mm / 1000) / 60
#[derive(Debug, Clone, Deserialize)]
pub struct ChassisConfig {
    /// 轮子直径（mm）
    pub wheel_diameter_mm: f64,
    /// 减速比（电机转 N 圈轮子转 1 圈，填 N）。PPR=4680 的 TT 马达典型 1:90
    pub gear_ratio: u32,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            camera: CameraConfig { width: 640, height: 480, fps: 30, jpeg_quality: 70 },
            motor: MotorConfig { backend: "dev".into(), port: "/dev/ttyS1".into(), baudrate: 115200, ppr: 4680 },
            arm: ArmConfig { backend: "dev".into(), port: "/dev/ttyS2".into(), baudrate: 115200 },
            web: WebConfig { port: 80 },
            chassis: ChassisConfig { wheel_diameter_mm: 62.0, gear_ratio: 90 },
            logging: LoggingConfig { level: "info".into() },
        }
    }
}

impl Config {
    /// 从当前目录的 config.toml 加载，文件不存在则返回默认值
    pub fn load() -> Self {
        std::fs::read_to_string("config.toml")
            .ok()
            .and_then(|s| toml::from_str(&s).ok())
            .unwrap_or_default()
    }
}
