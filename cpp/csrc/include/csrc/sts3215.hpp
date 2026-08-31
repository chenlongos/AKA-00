// csrc/sts3215.hpp — STS3215 总线舵机驱动（串口半双工协议）
//
// 对应 src/arm_control/sts3215/__init__.py：
//   帧格式: FF FF <id> <len> <inst> <params...> <chk>，chk = ~sum(data[2:]) & 0xFF
//   寄存器: 目标位置 0x2A(2B LE)、当前位置 0x38(2B LE)、速度 0x2E、最大扭矩 0x10、
//           保护电流 0x40、过载扭矩 0x24、运行模式 0x21、P/I/D 0x15/0x17/0x16

#pragma once

#include <cstdint>
#include <string>

#include "csrc/json.hpp"
#include "csrc/serial.hpp"

namespace csrc {

class STS3215 {
public:
    STS3215(const std::string& port = "/dev/ttyS2", int baudrate = 115200);

    bool ok() const { return ok_; }
    const std::string& error() const { return err_; }
    void close();

    /// 更新运行时角度配置（合并 grab_position / lift_position / gripper_*）
    void update_angles(const Json& angles);
    int pos(const std::string& group_key, const std::string& servo_key) const;
    int gripper_open_angle() const;
    int gripper_close_angle() const;

    // ── 底层指令 ──
    void send_cmd(uint8_t servo_id, uint8_t instruction, const uint8_t* params, size_t len);
    void write_reg(uint8_t servo_id, uint8_t addr, const uint8_t* data, size_t len);
    /// 读寄存器，返回 true + data
    bool read_data(uint8_t servo_id, uint8_t addr, size_t length, uint8_t* out, size_t* out_len);

    // ── 高层动作 ──
    void move_to_position(uint8_t servo_id, int pos);   // 0..4095
    bool get_position(uint8_t servo_id, int& pos);      // 当前角度，失败返回 false
    void move_angle(uint8_t servo_id, double angle);    // 0..360 → 4095 刻度
    void set_speed(uint8_t servo_id, int speed);
    void set_max_torque_limit(uint8_t servo_id, int torque);
    void set_protection_current(uint8_t servo_id, int torque);
    void set_overload_torque(uint8_t servo_id, int torque);
    void set_operating_mode(uint8_t servo_id, int mode);
    void set_p_coefficient(uint8_t servo_id, int v);
    void set_i_coefficient(uint8_t servo_id, int v);
    void set_d_coefficient(uint8_t servo_id, int v);

private:
    uint8_t checksum(const uint8_t* data, size_t len) const;

    SerialPort ser_;
    Json angles_;
    bool ok_ = false;
    std::string err_;
};

/// 初始化 3 个舵机（运行模式/速度/PID/扭矩限幅）
void sts3215_arm_init(STS3215& servo);
/// 抓取动作（张开 → 夹取位姿 → 闭合 → 抬起），与 Python grab() 对齐
void sts3215_grab(STS3215& servo);
/// 张开夹爪
void sts3215_release(STS3215& servo);

}  // namespace csrc
