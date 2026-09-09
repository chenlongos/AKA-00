// csrc/state.cpp

#include "csrc/state.hpp"

#include <chrono>

#include "csrc/log.hpp"
#include "csrc/system_utils.hpp"

namespace csrc {

StateCollector& StateCollector::get_instance() {
    static StateCollector inst;
    return inst;
}

void StateCollector::set_target_speed(int left, int right) {
    std::lock_guard<std::mutex> lk(mu_);
    status_.left_target = left;
    status_.right_target = right;
}

void StateCollector::set_gripper_target(int target) {
    std::lock_guard<std::mutex> lk(mu_);
    status_.gripper_target = target;
}

void StateCollector::set_gripper_status(const std::string& status) {
    std::lock_guard<std::mutex> lk(mu_);
    status_.gripper_status = status;
}

RobotStatus StateCollector::get_status() {
    std::lock_guard<std::mutex> lk(mu_);
    RobotStatus s = status_;
    s.timestamp_ms = (int64_t)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
    return s;
}

void StateCollector::start() {
    if (running_) return;
    running_ = true;
    thread_ = new std::thread([this] { loop(); });
    CAM_INFO("[state] collector started (10Hz)");
}

void StateCollector::stop() {
    running_ = false;
    if (thread_) {
        thread_->join();
        delete thread_;
        thread_ = nullptr;
    }
}

void StateCollector::loop() {
    constexpr double kInterval = 1.0 / 10.0;
    while (running_) {
        auto t0 = std::chrono::steady_clock::now();

        double left_speed = 0.0, right_speed = 0.0;
        if (motor_pair_) {
            int lr = 0, rr = 0;
            motor_pair_->get_speeds(lr, rr);
            // ESP32 固件返回的 rpm 已是轮速（编码器 4680 脉冲/轮圈、PWM_RPM_MAX=150，
            // 见 esp32_base_control/base_control.ino）——不要再除以齿轮比！
            // m/s = wheel_rpm × π × D / 60
            double wheel_rpm_l = (double)lr;
            double wheel_rpm_r = (double)rr;
            left_speed = wheel_rpm_l * 3.1415926535 * (wheel_diameter_mm_ / 1000.0) / 60.0;
            right_speed = wheel_rpm_r * 3.1415926535 * (wheel_diameter_mm_ / 1000.0) / 60.0;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_.left_speed = left_speed;
            status_.right_speed = right_speed;
            if (gripper_status_fn_) {
                status_.gripper_status = gripper_status_fn_();
            }
        }

        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        // 诊断：本应 100ms 一跳的状态循环若被拖 >400ms，说明单核被饿死——
        // WS 读不动(frame-timeout) / ESP32 心跳断(heartbeat lost)多由此而来
        if (elapsed > 0.4) {
            CAM_WARN("[state] loop lagged %.0fms (>400ms 应 100ms 一跳) cpu=%d%% — "
                     "单核被抢占，WS/串口轮询被饿死",
                     elapsed * 1000.0, csrc::cpu_usage());
        }
        double sleep = kInterval - elapsed;
        if (sleep > 0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(sleep));
        }
    }
}

}  // namespace csrc
