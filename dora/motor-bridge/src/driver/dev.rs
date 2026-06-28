//! 开发环境电机驱动 —— 打印命令到 stdout
//!
//! 对应 `src/base_control/interfaces.py` 的 MockMotorPair。

use super::MotorDriver;

pub struct DevMotor {
    left: f32,
    right: f32,
}

impl DevMotor {
    pub fn new() -> Self {
        Self { left: 0.0, right: 0.0 }
    }
}

impl MotorDriver for DevMotor {
    fn set_speeds(&mut self, left: i32, right: i32) {
        self.left = (left as f32) / 100.0;
        self.right = (right as f32) / 100.0;
        println!(
            "[motor-dev] set_speeds left={} right={} ({:.2} m/s, {:.2} m/s)",
            left, right, self.left, self.right
        );
    }

    fn stop(&mut self) {
        self.left = 0.0;
        self.right = 0.0;
        println!("[motor-dev] stop");
    }

    fn rpm(&self) -> (i32, i32) {
        (0, 0)
    }
}
