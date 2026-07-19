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
    #[serde(default = "default_https_port")]
    pub https_port: u16,
}

fn default_https_port() -> u16 { 5443 }

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
            web: WebConfig { port: 80, https_port: 5443 },
            chassis: ChassisConfig { wheel_diameter_mm: 62.0, gear_ratio: 90 },
            logging: LoggingConfig { level: "info".into() },
        }
    }
}

impl Config {
    /// 加载 config.toml，按优先级查找：
    ///   1. `$DORA_HOME/config.toml`（生产部署）
    ///   2. 二进制所在目录的 `../config.toml`（`bin/web-server` → `config.toml`）
    ///   3. CWD 下的 `config.toml`（开发 `dora/` 目录直接跑）
    ///
    /// 都找不到则 warn + 返回默认值（motor/arm backend=dev，不控制硬件）。
    pub fn load() -> Self {
        let path = Self::find_config_path();
        match std::fs::read_to_string(&path) {
            Ok(s) => match toml::from_str(&s) {
                Ok(cfg) => {
                    log::info!("[dora-config] loaded {}", path.display());
                    cfg
                }
                Err(e) => {
                    log::warn!("[dora-config] failed to parse {}: {e}; using defaults", path.display());
                    Self::default()
                }
            },
            Err(_) => {
                log::warn!(
                    "[dora-config] config.toml not found (tried DORA_HOME, ../ relative to binary, cwd); \
                     using defaults (motor/arm backend=dev — 不控制硬件)"
                );
                Self::default()
            }
        }
    }

    fn find_config_path() -> std::path::PathBuf {
        // 1. $DORA_HOME/etc/config.toml（生产部署：build_release.sh 打包到 etc/ 下）
        if let Ok(home) = std::env::var("DORA_HOME") {
            let p = std::path::PathBuf::from(&home).join("etc").join("config.toml");
            if p.exists() {
                return p;
            }
        }

        // 2. 二进制位置推算：bin/web-server → ../etc/config.toml（生产布局）
        if let Ok(exe) = std::env::current_exe() {
            if let Some(parent) = exe.parent() {
                // bin/ 下 → ../etc/config.toml
                if parent.file_name().map(|n| n == "bin").unwrap_or(false) {
                    if let Some(root) = parent.parent() {
                        let p = root.join("etc").join("config.toml");
                        if p.exists() {
                            return p;
                        }
                    }
                }
            }
        }

        // 3. CWD（开发时在 dora/ 目录下 `dora run dataflow.yml`）
        std::path::PathBuf::from("config.toml")
    }
}
