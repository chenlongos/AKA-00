//! 电机驱动抽象层 —— 屏蔽硬件细节，只暴露统一接口
//!
//! 对应 `src/base_control/interfaces.py` 的 MotorPairProtocol。

pub mod dev;
pub mod tt_pid;

/// 电机驱动统一接口（与 Python MotorPairProtocol 对应）
pub trait MotorDriver: Send + Sync {
    /// 设置左右轮速度 (-100 ~ 100)
    fn set_speeds(&mut self, left: i32, right: i32);
    /// 停止
    fn stop(&mut self);
    /// 读取当前 RPM（实际硬件返回真实值，dev 返回 0）
    #[allow(dead_code)]
    fn rpm(&self) -> (i32, i32) { (0, 0) }
    /// 重新初始化硬件（tt_pid ESP32 重置等）
    fn reinitialize(&mut self) -> bool { true }
}

/// 根据配置创建驱动实例
pub fn create_driver(config: &DriverConfig) -> Box<dyn MotorDriver> {
    match config.backend.as_deref() {
        Some("tt_pid") => {
            let port = config.port.as_deref().unwrap_or("/dev/ttyS1");
            let baudrate = config.baudrate.unwrap_or(115200);
            // TtPidDriver::new 内部自带串口失败回退到 stub 的逻辑，
            // 这样 dev 机上没接底盘时整个 dataflow 不会因为一个节点 panic 反复重启。
            tt_pid::TtPidDriver::new(port, baudrate, config.ppr.unwrap_or(4680))
        }
        _ => Box::new(dev::DevMotor::new()),
    }
}

#[derive(Debug, Clone, Default)]
pub struct DriverConfig {
    pub backend: Option<String>,
    pub port: Option<String>,
    pub baudrate: Option<u32>,
    pub ppr: Option<i32>,
}
