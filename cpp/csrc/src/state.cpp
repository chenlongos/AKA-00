// csrc/state.cpp

#include "csrc/state.hpp"

#include <chrono>

#include "csrc/log.hpp"

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
            // wheel_rpm = motor_rpm / gear_ratio; m/s = wheel_rpm * π * D / 60
            double wheel_rpm_l = (double)lr / gear_ratio_;
            double wheel_rpm_r = (double)rr / gear_ratio_;
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
        double sleep = kInterval - elapsed;
        if (sleep > 0) {
            std::this_thread::sleep_for(std::chrono::duration<double>(sleep));
        }
    }
}

}  // namespace csrc
