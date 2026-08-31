// csrc/gripper.hpp — 夹爪/机械臂抽象接口 + 适配器 + 工厂
//
// 对应 src/arm_control/interfaces.py：
//   - GripperProtocol: open / close / get_status / update_angles / preview_angle
//   - ZP10SGripperAdapter / STS3215GripperAdapter / MockGripper
//   - create_gripper(driver, port, baudrate)

#pragma once

#include <memory>
#include <string>

#include "csrc/json.hpp"

namespace csrc {

enum class GripperStatus { Open, Closed, Moving, Unknown };

inline const char* gripper_status_str(GripperStatus s) {
    switch (s) {
        case GripperStatus::Open: return "open";
        case GripperStatus::Closed: return "closed";
        case GripperStatus::Moving: return "moving";
        default: return "unknown";
    }
}

class Gripper {
public:
    virtual ~Gripper() = default;

    virtual void open() = 0;
    virtual void close() = 0;
    virtual GripperStatus get_status() = 0;
    virtual void update_angles(const Json& angles) = 0;
    /// 预览角度：key 如 "grab_position.servo0" / "gripper_open"，立即执行 set_angle
    virtual void preview_angle(const std::string& key, int angle) = 0;
    /// 发送原始命令到驱动串口（raw_command 路由用；默认 no-op）
    virtual void send_raw_cmd(const std::string&) {}
};

/// 创建夹爪驱动。driver: "zp10s" | "sts3215" | "dev"（mock）
std::unique_ptr<Gripper> create_gripper(const std::string& driver,
                                        const std::string& port = "/dev/ttyS2",
                                        int baudrate = 115200);

/// 解析 servo key 中的舵机 ID："xxx.servoN" → N；"servoN_..." → N；"servoN" → N。
/// gripper_open/gripper_close 无 ID（返回 -1，由调用方按 driver 决定夹爪舵机 ID）。
int resolve_servo_id(const std::string& key, int gripper_servo);

}  // namespace csrc
