// csrc/motor_pair.hpp — 双轮底盘抽象接口 + 工厂（对应 src/base_control/interfaces.py）

#pragma once

#include <memory>
#include <string>

#include "csrc/log.hpp"
#include "csrc/tt_pid.hpp"

namespace csrc {

class MotorPair {
public:
    virtual ~MotorPair() = default;

    virtual void set_speed(int left, int right) = 0;
    virtual void get_speeds(int& left_rpm, int& right_rpm) = 0;
    virtual void brake() = 0;
    virtual void sleep() = 0;
    virtual void close() = 0;
    virtual bool reinitialize() = 0;
    virtual void get_encoder(int& c1, int& c2) = 0;

    /// 闭环距离/转向（仅 tt_pid 支持，mock 忽略）
    virtual void move_distance(uint8_t dir, uint8_t speed, int32_t target) {}
    /// 发原始帧（仅 tt_pid 支持）
    virtual void send_cmd_noresp(uint8_t cmd, const uint8_t* payload, size_t len) {
        (void)cmd; (void)payload; (void)len;
    }
};

/// Mock 底盘（开发机，无真实硬件）：打印命令
class MockMotorPair : public MotorPair {
public:
    void set_speed(int left, int right) override {
        CAM_INFO("[MockMotorPair] set_speed(left=%d, right=%d)", left, right);
        last_left_ = left; last_right_ = right;
    }
    void get_speeds(int& l, int& r) override { l = last_left_; r = last_right_; }
    void brake() override { CAM_INFO("[MockMotorPair] brake()"); last_left_ = last_right_ = 0; }
    void sleep() override { CAM_INFO("[MockMotorPair] sleep()"); last_left_ = last_right_ = 0; }
    void close() override {}
    bool reinitialize() override { return true; }
    void get_encoder(int& c1, int& c2) override { c1 = c2 = 0; }

private:
    int last_left_ = 0;
    int last_right_ = 0;
};

/// 创建底盘。backend: "tt_pid"（ESP32 编码器）或 "dev"（开发用 mock）。
std::unique_ptr<MotorPair> create_motor_pair(const std::string& port,
                                             const std::string& backend,
                                             int baudrate = 115200, int ppr = 4680);

}  // namespace csrc
