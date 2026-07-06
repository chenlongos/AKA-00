//! 机械臂驱动抽象层 —— 屏蔽硬件细节，只暴露统一接口
//!
//! 对应 `src/arm_control/interfaces.py` 的 ServoProtocol/GripperProtocol。
//! ZP10S 协议细节见 `hardware/zp10s/zp10s_commands.txt` 与
//! `src/arm_control/zl/zp10s/uart_control.py`。

pub mod dev;
pub mod zp10s;

/// arm 状态上报（每次命令后立刻 publish）
#[derive(Debug, Clone, Default, serde::Serialize)]
pub struct ArmStatus {
    /// 每个舵机最近一次 set_angle 的角度（ID → 角度）。仅缓存命令意图，
    /// zp10s 是单向协议没有反馈，所以并不代表真实位置。
    pub angles: std::collections::BTreeMap<u8, u16>,
    /// "on" / "off" / "unknown"
    pub torque: String,
    /// 最近一次动作（前端调试用）
    pub last_action: String,
}

/// 机械臂驱动统一接口
pub trait ArmDriver: Send + Sync {
    /// 单舵机移动到指定角度（0~270）
    fn set_angle(&mut self, servo_id: u8, angle: u16);
    /// 批量移动（id, angle）
    fn set_angles(&mut self, moves: &[(u8, u16)]);
    /// 广播释放扭力（#255PULK!）
    fn release_torque(&mut self);
    /// 广播恢复扭力（#255PULR!）
    fn restore_torque(&mut self);
    /// 紧急停止（#255PDST!）
    fn stop(&mut self);
    /// 读取最近一次状态（dev/stub 缓存命令意图；zp10s 不读真实位置）
    fn status(&self) -> ArmStatus;
}

/// 根据配置创建驱动实例
pub fn create_driver(config: &DriverConfig) -> Box<dyn ArmDriver> {
    match config.backend.as_deref() {
        Some("zp10s") => {
            let port = config.port.as_deref().unwrap_or("/dev/ttyS2");
            let baudrate = config.baudrate.unwrap_or(115200);
            // Zp10sDriver::new 内部自带串口失败回退 stub 的逻辑，
            // 这样 dev 机上没接机械臂时整个 dataflow 不会因为一个节点 panic 反复重启。
            zp10s::Zp10sDriver::new(port, baudrate)
        }
        _ => Box::new(dev::DevArm::new()),
    }
}

#[derive(Debug, Clone, Default)]
pub struct DriverConfig {
    pub backend: Option<String>,
    pub port: Option<String>,
    pub baudrate: Option<u32>,
}