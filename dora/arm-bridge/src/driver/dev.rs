//! 开发环境机械臂驱动 —— 打印命令到 stdout
//!
//! 对应 `src/arm_control/interfaces.py` 的 MockGripper。

use std::sync::Mutex;

use super::{ArmDriver, ArmStatus};

pub struct DevArm {
    status: Mutex<ArmStatus>,
}

impl DevArm {
    pub fn new() -> Self {
        Self {
            status: Mutex::new(ArmStatus {
                angles: Default::default(),
                torque: "unknown".into(),
                last_action: String::new(),
            }),
        }
    }
}

impl Default for DevArm {
    fn default() -> Self {
        Self::new()
    }
}

impl ArmDriver for DevArm {
    fn set_angle(&mut self, servo_id: u8, angle: u16) {
        let angle = angle.min(270);
        println!("[arm-dev] set_angle id={} angle={}", servo_id, angle);
        let mut s = self.status.lock().unwrap();
        s.angles.insert(servo_id, angle);
        s.last_action = format!("set_angle {} {}", servo_id, angle);
    }

    fn set_angles(&mut self, moves: &[(u8, u16)]) {
        for &(id, angle) in moves {
            self.set_angle(id, angle);
        }
    }

    fn release_torque(&mut self) {
        println!("[arm-dev] release_torque (#255PULK!)");
        let mut s = self.status.lock().unwrap();
        s.torque = "off".into();
        s.last_action = "release_torque".into();
    }

    fn restore_torque(&mut self) {
        println!("[arm-dev] restore_torque (#255PULR!)");
        let mut s = self.status.lock().unwrap();
        s.torque = "on".into();
        s.last_action = "restore_torque".into();
    }

    fn stop(&mut self) {
        println!("[arm-dev] stop (#255PDST!)");
        let mut s = self.status.lock().unwrap();
        s.last_action = "stop".into();
    }

    fn status(&self) -> ArmStatus {
        self.status.lock().unwrap().clone()
    }
}