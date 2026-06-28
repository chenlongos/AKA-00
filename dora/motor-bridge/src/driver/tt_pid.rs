//! TT 马达 ESP32-C3 底盘 UART 控制器
//!
//! 对应 `src/base_control/tt_pid/__init__.py` 的 TtPidChassis。
//! 协议完全匹配：帧格式 0xAA 0x55 <cmd> <len> <payload> <chk>
//! 校验：cmd ^ len ^ payload[0] ^ ... ^ payload[last]

use std::io::{Read, Write};
use std::sync::Mutex;
use std::time::Duration;

use super::MotorDriver;

// ── 协议常量（与 Python 版本一致）──

const FRAME_H1: u8 = 0xAA;
const FRAME_H2: u8 = 0x55;

const CMD_INIT: u8 = 0x01;
const CMD_CONFIG: u8 = 0x02;
const CMD_SET_SPEEDS: u8 = 0x13;
const CMD_STOP: u8 = 0x11;

const RSP_ACK: u8 = 0x80;

/// TT PID 底盘驱动
pub struct TtPidDriver {
    port: Mutex<Box<dyn serialport::SerialPort>>,
}

impl TtPidDriver {
    pub fn new(port_path: &str, baudrate: u32, ppr: i32) -> Self {
        let port = serialport::new(port_path, baudrate)
            .data_bits(serialport::DataBits::Eight)
            .parity(serialport::Parity::None)
            .stop_bits(serialport::StopBits::One)
            .timeout(Duration::from_millis(100))
            .open()
            .unwrap_or_else(|e| panic!("Failed to open {}: {}", port_path, e));

        let driver = Self { port: Mutex::new(port) };
        driver.init();
        driver.config(ppr, 20000);
        driver
    }

    fn send_cmd(&self, cmd: u8, payload: &[u8]) {
        let len = payload.len() as u8;
        let mut chk = cmd ^ len;
        for &b in payload { chk ^= b; }

        let mut frame = vec![FRAME_H1, FRAME_H2, cmd, len];
        frame.extend_from_slice(payload);
        frame.push(chk);

        let mut port = self.port.lock().unwrap();
        let _ = port.write_all(&frame);
        let _ = port.flush();
    }

    fn read_ack(&self) -> bool {
        let mut buf = [0u8; 5];
        let mut port = self.port.lock().unwrap();
        match port.read_exact(&mut buf) {
            Ok(()) => buf[0] == FRAME_H1 && buf[1] == FRAME_H2 && buf[2] == RSP_ACK,
            Err(_) => false,
        }
    }

    fn init(&self) {
        self.send_cmd(CMD_INIT, &[]);
        let _ = self.read_ack();
    }

    fn config(&self, ppr: i32, pwm_freq: i32) {
        let p = [
            (ppr >> 24) as u8, (ppr >> 16) as u8,
            (ppr >> 8) as u8, ppr as u8,
            (pwm_freq >> 24) as u8, (pwm_freq >> 16) as u8,
            (pwm_freq >> 8) as u8, pwm_freq as u8,
        ];
        self.send_cmd(CMD_CONFIG, &p);
        let _ = self.read_ack();
    }
}

impl MotorDriver for TtPidDriver {
    fn set_speeds(&mut self, left: i32, right: i32) {
        let p = [(left >> 8) as u8, left as u8, (right >> 8) as u8, right as u8];
        self.send_cmd(CMD_SET_SPEEDS, &p);
    }

    fn stop(&mut self) {
        self.send_cmd(CMD_STOP, &[]);
    }
}
