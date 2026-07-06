//! ZP10S 舵机 UART 驱动
//!
//! 对应 `src/arm_control/zl/zp10s/uart_control.py` 的 ZP10S 类。
//! 协议（ASCII，fire-and-forget）：
//!     单舵机:   #<id:03d>P<pulse:04d>T<ms:04d>!
//!     广播:     #255<cmd>!        (PULK / PULR / PDST / PDPT / PDCT 等)
//!
//! 角度 → 脉宽：pulse = 500 + angle/270 * 2000，clamp 到 500..=2500。
//! 默认运动时间 1000ms（与 Python `_send_frame` 中的 T1000 一致）。
//!
//! 日志约定（统一走 stderr，方便 dora 捕获；通过 env_logger 控级别）：
//!   error!  — 硬错误（端口丢失、IO 错误）
//!   warn!   — 降级/异常（stub 回退）
//!   info!   — 生命周期（开串口、STOP 用户动作）
//!   debug!  — TX hex、set_angle 等高频信息
//!
//! 启用 DEBUG：在 shell 里 `export RUST_LOG=arm_bridge=debug` 再启动 dora。

use std::collections::BTreeMap;
use std::io::Write;
use std::sync::Mutex;
use std::time::Duration;

use log::{debug, info, warn};

use super::{ArmDriver, ArmStatus};

// ── 协议常量 ──

/// 广播 ID：扭力 / 停止 / 暂停 等指令的目标
const BROADCAST_ID: u8 = 255;

/// 脉宽范围（µs），与 Python `_send_frame` 中 `max(500, min(2500, pulse))` 对齐
const PULSE_MIN: i32 = 500;
const PULSE_MAX: i32 = 2500;

/// 默认运动时间（ms）
const DEFAULT_TIME_MS: u16 = 1000;

/// ZP10S 角度上限（0~270°）
const ANGLE_MAX: u16 = 270;

fn angle_to_pulse(angle: u16) -> i32 {
    let clamped = angle.min(ANGLE_MAX);
    // 与 Python `int(500 + (angle / 270.0) * 2000)` 数学等价（用 i32 避免溢出）
    PULSE_MIN + (clamped as i32) * 2000 / (ANGLE_MAX as i32)
}

/// 单舵机移动帧：`#<id>P<pulse>T<ms>!`
fn build_move_frame(id: u8, angle: u16) -> String {
    let pulse = angle_to_pulse(angle).clamp(PULSE_MIN, PULSE_MAX);
    // 整数默认就右对齐并填 0 —— `{:03}` / `{:04}` 已足够
    format!("#{:03}P{:04}T{:04}!", id, pulse, DEFAULT_TIME_MS)
}

/// 广播命令帧：`#255<cmd>!`
fn build_broadcast_frame(cmd: &str) -> String {
    format!("#{:03}{}!", BROADCAST_ID, cmd)
}

// ── 串口不可用时回退的桩驱动 ──
// 用于 dev 机上没接机械臂时让 arm-bridge 不挂掉。

struct StubDriver {
    status: Mutex<ArmStatus>,
}

impl StubDriver {
    fn new() -> Self {
        Self {
            status: Mutex::new(ArmStatus {
                angles: BTreeMap::new(),
                torque: "unknown".into(),
                last_action: String::new(),
            }),
        }
    }
}

impl ArmDriver for StubDriver {
    fn set_angle(&mut self, servo_id: u8, angle: u16) {
        let angle = angle.min(ANGLE_MAX);
        debug!(
            "[zp10s/stub] set_angle id={} angle={} (port unavailable, no-op)",
            servo_id, angle
        );
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
        debug!("[zp10s/stub] release_torque (port unavailable, no-op)");
        self.status.lock().unwrap().torque = "off".into();
    }

    fn restore_torque(&mut self) {
        debug!("[zp10s/stub] restore_torque (port unavailable, no-op)");
        self.status.lock().unwrap().torque = "on".into();
    }

    fn stop(&mut self) {
        debug!("[zp10s/stub] stop (port unavailable, no-op)");
        self.status.lock().unwrap().last_action = "stop".into();
    }

    fn status(&self) -> ArmStatus {
        self.status.lock().unwrap().clone()
    }
}

// ── ZP10S 真实驱动 ──

pub struct Zp10sDriver {
    port: Mutex<Option<Box<dyn serialport::SerialPort>>>,
    status: Mutex<ArmStatus>,
}

impl Zp10sDriver {
    pub fn new(port_path: &str, baudrate: u32) -> Box<dyn ArmDriver> {
        info!(
            "[zp10s] opening port {} @ {} baud (8N1)...",
            port_path, baudrate
        );
        let port = match serialport::new(port_path, baudrate)
            .data_bits(serialport::DataBits::Eight)
            .parity(serialport::Parity::None)
            .stop_bits(serialport::StopBits::One)
            .timeout(Duration::from_millis(100))
            .open()
        {
            Ok(p) => p,
            Err(e) => {
                warn!(
                    "[zp10s] open {} @ {} baud failed: {}. Falling back to stub driver — \
                     arm commands will be logged but NOT sent. Check cable / port path / permissions.",
                    port_path, baudrate, e
                );
                return Box::new(StubDriver::new());
            }
        };

        info!("[zp10s] ✓ port {} opened", port_path);

        Box::new(Self {
            port: Mutex::new(Some(port)),
            status: Mutex::new(ArmStatus {
                angles: BTreeMap::new(),
                torque: "on".into(),
                last_action: String::new(),
            }),
        })
    }

    /// ZServo 是单向协议（写完不等应答，Python 也不读），所以这里只 write+flush。
    /// 失败打 error! 但不重试——上层如果需要重发可以再发一次。
    fn send(&self, frame: &str) {
        let bytes = frame.as_bytes();
        let mut guard = self.port.lock().unwrap();
        match guard.as_mut() {
            None => {
                warn!("[zp10s] tx called but port is None (stub mode?)");
            }
            Some(p) => {
                if let Err(e) = p.write_all(bytes) {
                    log::error!("[zp10s] tx write failed: {} (frame={:?})", e, frame);
                    return;
                }
                if let Err(e) = p.flush() {
                    log::error!("[zp10s] tx flush failed: {} (frame={:?})", e, frame);
                    return;
                }
                debug!("[zp10s/tx] {:?}", frame);
            }
        }
    }
}

impl ArmDriver for Zp10sDriver {
    fn set_angle(&mut self, servo_id: u8, angle: u16) {
        let angle = angle.min(ANGLE_MAX);
        let frame = build_move_frame(servo_id, angle);
        debug!("[zp10s] set_angle id={} angle={} -> {}", servo_id, angle, frame);
        self.send(&frame);
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
        let frame = build_broadcast_frame("PULK");
        info!("[zp10s] release_torque -> {}", frame);
        self.send(&frame);
        let mut s = self.status.lock().unwrap();
        s.torque = "off".into();
        s.last_action = "release_torque".into();
    }

    fn restore_torque(&mut self) {
        let frame = build_broadcast_frame("PULR");
        info!("[zp10s] restore_torque -> {}", frame);
        self.send(&frame);
        let mut s = self.status.lock().unwrap();
        s.torque = "on".into();
        s.last_action = "restore_torque".into();
    }

    fn stop(&mut self) {
        info!("[zp10s] stop (broadcast PDST)");
        let frame = build_broadcast_frame("PDST");
        self.send(&frame);
        let mut s = self.status.lock().unwrap();
        s.last_action = "stop".into();
    }

    fn status(&self) -> ArmStatus {
        self.status.lock().unwrap().clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn angle_to_pulse_extremes() {
        assert_eq!(angle_to_pulse(0), 500);
        assert_eq!(angle_to_pulse(270), 2500);
        assert_eq!(angle_to_pulse(135), 500 + 135 * 2000 / 270); // 1500
    }

    #[test]
    fn angle_to_pulse_clamps_over_range() {
        // 超出 270° 视为 270°（与 Python max(500, min(2500, pulse)) 行为一致）
        assert_eq!(angle_to_pulse(300), 2500);
        assert_eq!(angle_to_pulse(500), 2500);
    }

    #[test]
    fn move_frame_format() {
        assert_eq!(build_move_frame(0, 150), "#000P1611T1000!");
        assert_eq!(build_move_frame(2, 90), "#002P1166T1000!");
        // 270° → pulse 2500
        assert_eq!(build_move_frame(1, 270), "#001P2500T1000!");
    }

    #[test]
    fn broadcast_frame_format() {
        assert_eq!(build_broadcast_frame("PULK"), "#255PULK!");
        assert_eq!(build_broadcast_frame("PDST"), "#255PDST!");
        assert_eq!(build_broadcast_frame("PULR"), "#255PULR!");
    }
}