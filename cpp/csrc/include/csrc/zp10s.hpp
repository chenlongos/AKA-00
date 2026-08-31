// csrc/zp10s.hpp — ZP10S 舵机控制板驱动（UART ASCII 协议）
//
// 对应 src/arm_control/zl/zp10s/uart_control.py：
//   - set_angle: 角度(0..270) → 脉宽 500..2500us → "#{id:03d}P{pulse:04d}T1000!"
//   - 力矩开关: "#255PULK" (release) / "#255PULR" (restore)
// 高层 grab / release 由本驱动读 arm_angles.json 后展开成 set_angle 序列。

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "csrc/json.hpp"
#include "csrc/serial.hpp"

namespace csrc {

class ZP10S {
public:
    ZP10S(const std::string& port = "/dev/ttyS2", int baudrate = 115200);

    bool ok() const { return ok_; }
    const std::string& error() const { return err_; }

    void close();
    bool is_open() const { return ser_.is_open(); }

    /// 设置角度（0~270°），映射到脉宽 500~2500us
    bool set_angle(int servo_id, double angle);
    void release_torque();    // #255PULK
    void restoring_torque();  // #255PULR

    /// 更新运行时角度配置（合并 grab_position / lift_position / gripper_*）
    void update_angles(const Json& angles);

    /// 读某个位姿组中某个舵机的角度（默认 150）
    int pos(const std::string& group_key, const std::string& servo_key) const;

    int gripper_open_angle() const;
    int gripper_close_angle() const;

    /// 发原始 ASCII 命令（raw_command 路由用）
    void send_raw_cmd(const std::string& cmd);

private:
    void send_frame(int servo_id, double angle);
    void send_cmd_str(int servo_id, const std::string& cmd);
    void load_angles();

    SerialPort ser_;
    Json angles_;
    bool ok_ = false;
    std::string err_;
};

/// 抓取动作（张开 → 夹取位姿 → 闭合 → 抬起）
void zp10s_grab(ZP10S& servo, const std::string& driver = "zp10s");
/// 张开夹爪
void zp10s_release(ZP10S& servo);

}  // namespace csrc
