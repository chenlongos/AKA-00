//! TT 马达 ESP32-C3 底盘 UART 控制器
//!
//! 对应 `src/base_control/tt_pid/__init__.py` 的 TtPidChassis。
//! 协议完全匹配：帧格式 0xAA 0x55 <cmd> <len> <payload...> <chk>
//! 校验：cmd ^ len ^ payload[0] ^ ... ^ payload[last]
//!
//! 日志约定（统一走 stderr，方便 dora 捕获）：
//!   [motor-tt_pid]      — 生命周期 / 状态变更
//!   [motor-tt_pid/tx]   — 发出的帧（hex）
//!   [motor-tt_pid/rx]   — 收到的帧（hex）或超时
//!   [motor-tt_pid/err]  — 错误 / 降级
//!
//! 想静音可设环境变量 `MOTOR_TT_PID_QUIET=1`（仍保留 error/降级日志）。

use std::io::{Read, Write};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use super::MotorDriver;

// ── 协议常量（与 Python 版本一致）──

const FRAME_H1: u8 = 0xAA;
const FRAME_H2: u8 = 0x55;

const CMD_INIT: u8 = 0x01;
const CMD_CONFIG: u8 = 0x02;
const CMD_SET_SPEEDS: u8 = 0x13;
const CMD_STOP: u8 = 0x11;
const CMD_GET_STATUS: u8 = 0x21;

const RSP_ACK: u8 = 0x80;
const RSP_NACK: u8 = 0x81;
const RSP_STATUS: u8 = 0x91;

/// 电机速度范围（与 ESP32 固件 `constrain(spd, -100, 100)` 对齐）
const SPEED_MIN: i32 = -100;
const SPEED_MAX: i32 = 100;

/// STOP payload 中"双电机同时停"的魔数（固件 p[0]==2 走 motorCoast(0)+motorCoast(1)）
const STOP_BOTH: u8 = 2;

// ── 日志工具 ──

fn quiet() -> bool {
    std::env::var("MOTOR_TT_PID_QUIET")
        .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
        .unwrap_or(false)
}

fn hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 3);
    for (i, b) in bytes.iter().enumerate() {
        if i > 0 {
            s.push(' ');
        }
        s.push_str(&format!("{:02X}", b));
    }
    s
}

fn cmd_name(cmd: u8) -> &'static str {
    match cmd {
        CMD_INIT => "INIT",
        CMD_CONFIG => "CONFIG",
        CMD_SET_SPEEDS => "SET_SPEEDS",
        CMD_STOP => "STOP",
        CMD_GET_STATUS => "GET_STATUS",
        _ => "?",
    }
}

fn rsp_name(rsp: u8) -> &'static str {
    match rsp {
        RSP_ACK => "ACK",
        RSP_NACK => "NACK",
        _ => "?",
    }
}

// ── 串口不可用时回退的桩驱动 ──
// 用于 dev 机上没接底盘时让 motor-bridge 不挂掉。

struct StubDriver;

impl MotorDriver for StubDriver {
    fn set_speeds(&mut self, left: i32, right: i32) {
        eprintln!(
            "[motor-tt_pid/stub] set_speeds left={} right={} (port unavailable, no-op)",
            left, right
        );
    }
    fn stop(&mut self) {
        eprintln!("[motor-tt_pid/stub] stop (port unavailable, no-op)");
    }
}

// ── TT PID 底盘驱动 ──

pub struct TtPidDriver {
    port: Mutex<Option<Box<dyn serialport::SerialPort>>>,
}

impl TtPidDriver {
    pub fn new(port_path: &str, baudrate: u32, ppr: i32) -> Box<dyn MotorDriver> {
        eprintln!(
            "[motor-tt_pid] opening port {} @ {} baud (8N1)...",
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
                eprintln!(
                    "[motor-tt_pid/err] open {} @ {} baud failed: {}. \
                     Falling back to stub driver — wheel commands will be logged but NOT sent. \
                     Check cable / port path / permissions.",
                    port_path, baudrate, e
                );
                return Box::new(StubDriver);
            }
        };

        eprintln!(
            "[motor-tt_pid] ✓ port {} opened, ppr={} pwm_freq=20000",
            port_path, ppr
        );

        let driver = Self {
            port: Mutex::new(Some(port)),
        };

        // 等 ESP32 USB-CDC 枚举完成（参照 Python `time.sleep(0.5)`）
        eprintln!("[motor-tt_pid] waiting 500ms for ESP32 USB-CDC enumeration...");
        std::thread::sleep(Duration::from_millis(500));

        // 清掉 boot log / 复位消息残留（参照 Python `reset_input_buffer`）
        {
            let mut guard = driver.port.lock().unwrap();
            if let Some(p) = guard.as_mut() {
                if let Err(e) = p.clear(serialport::ClearBuffer::Input) {
                    eprintln!(
                        "[motor-tt_pid/err] clear input buffer failed: {} (non-fatal, continuing)",
                        e
                    );
                } else {
                    eprintln!("[motor-tt_pid] input buffer cleared");
                }
            }
        }

        eprintln!("[motor-tt_pid] >>> handshake start");
        driver.init();
        driver.config(ppr, 20000);
        eprintln!("[motor-tt_pid] <<< handshake done, driver ready");

        Box::new(driver)
    }

    fn send_cmd(&self, cmd: u8, payload: &[u8]) {
        let len = payload.len() as u8;
        let mut chk = cmd ^ len;
        for &b in payload {
            chk ^= b;
        }

        let mut frame = vec![FRAME_H1, FRAME_H2, cmd, len];
        frame.extend_from_slice(payload);
        frame.push(chk);

        let name = cmd_name(cmd);
        let mut guard = self.port.lock().unwrap();
        match guard.as_mut() {
            None => {
                eprintln!(
                    "[motor-tt_pid/err] tx {} called but port is None (stub mode?)",
                    name
                );
            }
            Some(port) => {
                // 发命令前清输入缓冲，避免上次响应残留 / 启动 boot log 干扰本次解析
                // （对应 Python `_send_cmd` 里的 `reset_input_buffer()`）
                if let Err(e) = port.clear(serialport::ClearBuffer::Input) {
                    eprintln!(
                        "[motor-tt_pid/err] tx {} clear input buffer failed: {}",
                        name, e
                    );
                }
                if let Err(e) = port.write_all(&frame) {
                    eprintln!("[motor-tt_pid/err] tx {} write failed: {}", name, e);
                    return;
                }
                if let Err(e) = port.flush() {
                    eprintln!("[motor-tt_pid/err] tx {} flush failed: {}", name, e);
                    return;
                }
                if !quiet() {
                    eprintln!("[motor-tt_pid/tx] {} len={} -> {}", name, len, hex(&frame));
                }
            }
        }
    }

    /// 读一个完整的协议帧：`AA 55 <rsp> <len> [len 字节 payload] <chk>`。
    /// 成功返回 `Some((rsp_cmd, payload))`；任何错误（端口没开/超时/坏头/chk 错）返回 `None` 并打日志。
    fn read_frame(&self, label: &str) -> Option<(u8, Vec<u8>)> {
        let started = Instant::now();
        let mut guard = self.port.lock().unwrap();
        let port = match guard.as_mut() {
            Some(p) => p,
            None => {
                eprintln!(
                    "[motor-tt_pid/err] read_frame({}) called but port is None",
                    label
                );
                return None;
            }
        };

        // 4 字节头
        let mut header = [0u8; 4];
        if let Err(e) = port.read_exact(&mut header) {
            let elapsed_ms = started.elapsed().as_millis();
            eprintln!(
                "[motor-tt_pid/rx] {} header TIMEOUT after {}ms: {} (ESP32 not responding?)",
                label, elapsed_ms, e
            );
            return None;
        }
        if header[0] != FRAME_H1 || header[1] != FRAME_H2 {
            eprintln!(
                "[motor-tt_pid/rx] {} bad header: {} (want AA 55 ?? ??)",
                label,
                hex(&header)
            );
            return None;
        }
        let rsp = header[2];
        let len = header[3] as usize;

        // payload
        let mut payload = vec![0u8; len];
        if len > 0 {
            if let Err(e) = port.read_exact(&mut payload) {
                let elapsed_ms = started.elapsed().as_millis();
                eprintln!(
                    "[motor-tt_pid/rx] {} payload TIMEOUT after {}ms (len={}): {}",
                    label, elapsed_ms, len, e
                );
                return None;
            }
        }

        // chk
        let mut chk_byte = [0u8; 1];
        if let Err(e) = port.read_exact(&mut chk_byte) {
            let elapsed_ms = started.elapsed().as_millis();
            eprintln!(
                "[motor-tt_pid/rx] {} chk TIMEOUT after {}ms: {}",
                label, elapsed_ms, e
            );
            return None;
        }

        // 校验 chk
        let mut expected = rsp ^ (len as u8);
        for &b in &payload {
            expected ^= b;
        }
        if expected != chk_byte[0] {
            eprintln!(
                "[motor-tt_pid/rx] {} chk mismatch: got {:02X} expected {:02X} \
                 (rsp=0x{:02X} len={} payload={})",
                label,
                chk_byte[0],
                expected,
                rsp,
                len,
                hex(&payload)
            );
            return None;
        }

        let elapsed_ms = started.elapsed().as_millis();
        let quiet_ok = quiet() && rsp == RSP_ACK;
        if !quiet_ok {
            eprintln!(
                "[motor-tt_pid/rx] {} {} in {}ms, payload=[{}] (len={})",
                label,
                rsp_name(rsp),
                elapsed_ms,
                hex(&payload),
                len
            );
        }

        Some((rsp, payload))
    }

    fn init(&self) {
        self.send_cmd(CMD_INIT, &[]);
        match self.read_frame("INIT") {
            Some((RSP_ACK, payload)) => {
                if !payload.is_empty() {
                    eprintln!(
                        "[motor-tt_pid] INIT ACK payload=[{}] ({} bytes) — status code from firmware?",
                        hex(&payload),
                        payload.len()
                    );
                }
            }
            Some((RSP_NACK, payload)) => {
                eprintln!(
                    "[motor-tt_pid/err] INIT NACK payload=[{}] (firmware rejected init)",
                    hex(&payload)
                );
            }
            Some((other, payload)) => {
                eprintln!(
                    "[motor-tt_pid/err] INIT unexpected rsp=0x{:02X} payload=[{}]",
                    other,
                    hex(&payload)
                );
            }
            None => {
                eprintln!(
                    "[motor-tt_pid/err] INIT did not get a valid response. \
                     Common causes: wrong baudrate, ESP32 in download mode (GPIO0 low), \
                     wrong firmware, or /dev/tty path points to a different device."
                );
            }
        }
    }

    fn config(&self, ppr: i32, pwm_freq: i32) {
        // 严格按 Python `_config`: `struct.pack(">HH", ppr, pwm_freq)` —— 2 个大端 u16，共 4 字节。
        // 之前 i32 拆 8 字节会让 ESP32 固件 chk 对不上 → NACK。
        let ppr_u16 = ppr as u16;
        let freq_u16 = pwm_freq as u16;
        let p = [
            (ppr_u16 >> 8) as u8,
            ppr_u16 as u8,
            (freq_u16 >> 8) as u8,
            freq_u16 as u8,
        ];
        self.send_cmd(CMD_CONFIG, &p);
        match self.read_frame("CONFIG") {
            Some((RSP_ACK, payload)) => {
                if !payload.is_empty() {
                    eprintln!(
                        "[motor-tt_pid] CONFIG ACK payload=[{}] ({} bytes)",
                        hex(&payload),
                        payload.len()
                    );
                }
            }
            Some((RSP_NACK, payload)) => {
                eprintln!(
                    "[motor-tt_pid/err] CONFIG NACK payload=[{}] (firmware rejected ppr={} pwm_freq={})",
                    hex(&payload),
                    ppr,
                    pwm_freq
                );
            }
            Some((other, payload)) => {
                eprintln!(
                    "[motor-tt_pid/err] CONFIG unexpected rsp=0x{:02X} payload=[{}]",
                    other,
                    hex(&payload)
                );
            }
            None => {
                eprintln!(
                    "[motor-tt_pid/err] CONFIG did not get a valid response. \
                     Check ppr={} and pwm_freq={} are accepted by firmware.",
                    ppr, pwm_freq
                );
            }
        }
    }
}

impl MotorDriver for TtPidDriver {
    fn set_speeds(&mut self, left: i32, right: i32) {
        let l = left.clamp(SPEED_MIN, SPEED_MAX);
        let r = right.clamp(SPEED_MIN, SPEED_MAX);
        if !quiet() {
            eprintln!("[motor-tt_pid] set_speeds left={} right={}", l, r);
        }
        let p = [
            (l >> 8) as u8,
            l as u8,
            (r >> 8) as u8,
            r as u8,
        ];
        self.send_cmd(CMD_SET_SPEEDS, &p);
    }

    fn stop(&mut self) {
        eprintln!("[motor-tt_pid] stop (both motors coast)");
        // 固件 p[0]==2 表示双电机同时滑行停止
        self.send_cmd(CMD_STOP, &[STOP_BOTH]);
    }

    /// 读 ESP32 实时状态，返回 (M1_RPM, M2_RPM)。
    /// 失败（超时 / NACK / 状态机错）返回 (0, 0)，避免上层 UI 卡住。
    fn rpm(&self) -> (i32, i32) {
        self.send_cmd(CMD_GET_STATUS, &[]);
        match self.read_frame("GET_STATUS") {
            Some((RSP_STATUS, payload)) if payload.len() == 5 => {
                // payload: [status, M1_rpm_hi, M1_rpm_lo, M2_rpm_hi, M2_rpm_lo]
                let status = payload[0];
                let m1 = i16::from_be_bytes([payload[1], payload[2]]) as i32;
                let m2 = i16::from_be_bytes([payload[3], payload[4]]) as i32;
                if !quiet() {
                    eprintln!(
                        "[motor-tt_pid] status=0x{:02X} M1={} RPM M2={} RPM",
                        status, m1, m2
                    );
                }
                (m1, m2)
            }
            Some((rsp, payload)) => {
                eprintln!(
                    "[motor-tt_pid/err] GET_STATUS unexpected rsp=0x{:02X} payload=[{}] (len={})",
                    rsp,
                    hex(&payload),
                    payload.len()
                );
                (0, 0)
            }
            None => (0, 0),
        }
    }
}
