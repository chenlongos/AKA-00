// csrc/state.hpp — 机器人状态采集（对应 src/state/__init__.py 的 StateCollector）
//
// 单例 + 独立线程，10Hz 轮询电机 RPM 并换算线速度 m/s。
// 换算公式（wheel_diameter_mm / gear_ratio 来自 config.toml [chassis]）：
//   wheel_rpm = motor_rpm / gear_ratio
//   m/s       = wheel_rpm × π × (wheel_diameter_mm / 1000) / 60

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "csrc/motor_pair.hpp"

namespace csrc {

struct RobotStatus {
    double left_speed = 0.0;      // m/s
    double right_speed = 0.0;     // m/s
    int left_target = 0;          // PWM% (-100..100)
    int right_target = 0;
    std::string gripper_status = "open";  // open / closed / moving / unknown
    int gripper_target = 0;       // 0=释放, 1=夹取
    int64_t timestamp_ms = 0;
};

class StateCollector {
public:
    static StateCollector& get_instance();

    void set_motor_pair(MotorPair* motor_pair) { motor_pair_ = motor_pair; }
    void set_gripper_status_provider(std::function<std::string()> fn) { gripper_status_fn_ = std::move(fn); }
    void clear_gripper_status_provider() { gripper_status_fn_ = nullptr; }

    void set_target_speed(int left, int right);
    void set_gripper_target(int target);
    void set_gripper_status(const std::string& status);

    /// 快照当前状态（带时间戳）
    RobotStatus get_status();

    void start();
    void stop();

    void set_wheel_diameter_mm(double mm) { wheel_diameter_mm_ = mm; }
    void set_gear_ratio(int ratio) { gear_ratio_ = ratio; }

private:
    StateCollector() = default;
    void loop();

    std::mutex mu_;
    RobotStatus status_;
    bool running_ = false;
    std::thread* thread_ = nullptr;

    MotorPair* motor_pair_ = nullptr;
    std::function<std::string()> gripper_status_fn_;

    double wheel_diameter_mm_ = 62.0;
    int gear_ratio_ = 90;
};

}  // namespace csrc
