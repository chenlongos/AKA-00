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

    /// 读取当前位置（脉宽 500~2500）。手册 p.26 第 9 条：#000PRAD! → #000P1500!
    /// 超时/回包不合法（如 "#000P!" 这类无位置回包）返回 false。
    bool read_position(int servo_id, int& pulse);

    /// 闭环闭合夹爪：一边驱动一边用 read_position 盯位置，位置不再变（≈ 已经夹住物体）
    /// 就收手 —— 把目标改到"夹住处再往闭合方向 bias"，不再持续硬顶。
    ///
    /// 为什么：夹住东西 = 舵机永远到不了目标 = 持续堵转，而堵转电流 1.8~2A（手册
    /// p.7/p.14），手册每章的注意事项都写着"合理运行转矩≈1/3 堵转扭矩"。硬顶下去
    /// 要么触发防堵转释力（= 松劲），要么烧。收手之后靠位置误差维持夹持力，
    /// 电流随 bias 走 —— bias 就是"夹多紧"那个旋钮。
    ///
    /// 返回夹住时的脉宽；没夹到东西（正常走到目标）返回 -1。
    int grip_until_stall(int servo_id, double angle, int bias, int timeout_ms);

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
    void send_pulse(int servo_id, int pulse, int time_ms);
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
