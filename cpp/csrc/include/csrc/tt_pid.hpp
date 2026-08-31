// csrc/tt_pid.hpp — TT 马达 ESP32-C3 底盘 UART 控制器
//
// 对应 src/base_control/tt_pid/__init__.py 的 TtPidChassis。
// 帧格式: 0xAA 0x55 <cmd> <len> <payload...> <chk>，chk = cmd ^ len ^ payload[0..]
//
// 实现 MotorPair 接口（见 motor_pair.hpp），可与 Mock 互换。

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "csrc/serial.hpp"

namespace csrc {

// ── 协议常量 ──
inline constexpr uint8_t FRAME_H1 = 0xAA;
inline constexpr uint8_t FRAME_H2 = 0x55;

inline constexpr uint8_t CMD_INIT = 0x01;
inline constexpr uint8_t CMD_CONFIG = 0x02;
inline constexpr uint8_t CMD_SET_SPEED = 0x10;
inline constexpr uint8_t CMD_SET_SPEEDS = 0x13;
inline constexpr uint8_t CMD_STOP = 0x11;
inline constexpr uint8_t CMD_BRAKE = 0x12;
inline constexpr uint8_t CMD_GET_RPM = 0x20;
inline constexpr uint8_t CMD_GET_STATUS = 0x21;
inline constexpr uint8_t CMD_GET_ENCODER = 0x22;
inline constexpr uint8_t CMD_MOVE_DISTANCE = 0x23;  // ESP32 内置闭环距离控制
inline constexpr uint8_t CMD_RESET = 0xFF;

inline constexpr uint8_t RSP_ACK = 0x80;
inline constexpr uint8_t RSP_NACK = 0x81;
inline constexpr uint8_t RSP_RPM_DATA = 0x90;
inline constexpr uint8_t RSP_STATUS = 0x91;

class TtPidChassis {
public:
    TtPidChassis(const std::string& port = "/dev/ttyS1", int baudrate = 115200,
                 int ppr = 4680, int pwm_freq = 20000);
    ~TtPidChassis();

    bool ok() const { return ok_; }
    const std::string& error() const { return err_; }

    // ── MotorPair 接口 ──
    void set_speed(int left, int right);          // 百分比 PWM (-100..100)
    void brake();                                  // 刹车两个电机
    void sleep();                                  // 滑行停止两个电机
    void close();
    bool reinitialize();                           // 重新 INIT + CONFIG
    void get_speeds(int& left_rpm, int& right_rpm);  // 实时 RPM
    void get_encoder(int& c1, int& c2);            // 编码器累计脉冲 (M1, M2)

    /// 闭环距离/转向（ESP32 固件内部换算，直接发 mm / 0.1°）
    void move_distance(uint8_t dir, uint8_t speed, int32_t target);

    /// 低层：发原始帧（0x23 等），fire-and-forget
    void send_cmd_noresp(uint8_t cmd, const uint8_t* payload, size_t len);

    /// 低层：发命令并等响应，返回响应 cmd（-1 失败）
    int send_cmd(uint8_t cmd, const uint8_t* payload, size_t len,
                 uint8_t* rsp_payload, size_t* rsp_len, double timeout = 0.2);

private:
    bool init_chassis();
    bool config(int ppr, int pwm_freq);
    bool recv_frame(uint8_t* rsp, uint8_t* payload, size_t* payload_len, double timeout);

    SerialPort ser_;
    int ppr_;
    int pwm_freq_;
    bool ok_ = false;
    std::string err_;
    /// 命令序列级锁：状态轮询线程（StateCollector 10Hz get_speeds）与控制线程
    /// （set_speed/brake/move_distance）并发读写同一串口，必须保证
    /// clear+write(+read) 整体原子，否则帧字节交错被 ESP32 丢弃。
    std::mutex io_mu_;
};

}  // namespace csrc
