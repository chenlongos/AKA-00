// csrc/gripper.cpp

#include "csrc/gripper.hpp"

#include <cstdio>
#include <cstdlib>
#include <regex>
#include <thread>

#include "csrc/angle_config.hpp"
#include "csrc/log.hpp"
#include "csrc/sts3215.hpp"
#include "csrc/zp10s.hpp"

namespace csrc {

namespace {

// ── Mock ──
class MockGripper : public Gripper {
public:
    void open() override { CAM_INFO("[MockGripper] open()"); status_ = GripperStatus::Open; }
    void close() override { CAM_INFO("[MockGripper] close()"); status_ = GripperStatus::Closed; }
    GripperStatus get_status() override { return status_; }
    void update_angles(const Json&) override {}
    void preview_angle(const std::string& key, int angle) override {
        CAM_INFO("[MockGripper] preview_angle(%s=%d)", key.c_str(), angle);
    }

private:
    GripperStatus status_ = GripperStatus::Unknown;
};

// ── ZP10S 适配器 ──
class ZP10SGripperAdapter : public Gripper {
public:
    explicit ZP10SGripperAdapter(std::unique_ptr<ZP10S> zp10s) : zp10s_(std::move(zp10s)) {}

    void open() override { zp10s_release(*zp10s_); status_ = GripperStatus::Open; }
    void close() override { zp10s_grab(*zp10s_); status_ = GripperStatus::Closed; }
    GripperStatus get_status() override { return status_; }
    void update_angles(const Json& angles) override { zp10s_->update_angles(angles); }
    void preview_angle(const std::string& key, int angle) override {
        int servo_id = resolve_servo_id(key, /*gripper_servo=*/2);
        if (servo_id >= 0) zp10s_->set_angle(servo_id, angle);
    }
    void send_raw_cmd(const std::string& cmd) override { zp10s_->send_raw_cmd(cmd); }

private:
    std::unique_ptr<ZP10S> zp10s_;
    GripperStatus status_ = GripperStatus::Unknown;
};

// ── STS3215 适配器 ──
class STS3215GripperAdapter : public Gripper {
public:
    explicit STS3215GripperAdapter(std::unique_ptr<STS3215> servo) : servo_(std::move(servo)) {}

    void open() override { sts3215_release(*servo_); }
    void close() override { sts3215_grab(*servo_); }
    GripperStatus get_status() override {
        int pos = 0;
        if (!servo_->get_position(3, pos)) return GripperStatus::Unknown;
        if (pos > 3500) return GripperStatus::Open;
        if (pos < 2800) return GripperStatus::Closed;
        return GripperStatus::Moving;
    }
    void update_angles(const Json& angles) override { servo_->update_angles(angles); }
    void preview_angle(const std::string& key, int angle) override {
        int servo_id = resolve_servo_id(key, /*gripper_servo=*/3);
        if (servo_id >= 0) servo_->move_to_position((uint8_t)servo_id, angle);
    }

private:
    std::unique_ptr<STS3215> servo_;
};

}  // namespace

int resolve_servo_id(const std::string& key, int gripper_servo) {
    if (key == "gripper_open" || key == "gripper_close") return gripper_servo;
    // 新格式: "xxx.servoN"
    std::smatch m;
    if (std::regex_search(key, m, std::regex(R"(\.servo(\d+)$)"))) {
        return std::atoi(m[1].str().c_str());
    }
    // 旧格式: "servoN_..." / "servoN"
    if (std::regex_search(key, m, std::regex(R"(^servo(\d+))"))) {
        return std::atoi(m[1].str().c_str());
    }
    CAM_WARN("[gripper] invalid servo key: %s", key.c_str());
    return -1;
}

std::unique_ptr<Gripper> create_gripper(const std::string& driver,
                                        const std::string& port, int baudrate) {
    if (driver == "zp10s") {
        auto zp10s = std::make_unique<ZP10S>(port, baudrate);
        if (!zp10s->ok()) {
            CAM_WARN("[gripper] zp10s init failed (%s), falling back to mock", zp10s->error().c_str());
            return std::make_unique<MockGripper>();
        }
        return std::make_unique<ZP10SGripperAdapter>(std::move(zp10s));
    }
    if (driver == "sts3215") {
        auto servo = std::make_unique<STS3215>(port, baudrate);
        if (!servo->ok()) {
            CAM_WARN("[gripper] sts3215 init failed (%s), falling back to mock", servo->error().c_str());
            return std::make_unique<MockGripper>();
        }
        return std::make_unique<STS3215GripperAdapter>(std::move(servo));
    }
    CAM_INFO("[gripper] driver=%s → mock", driver.c_str());
    return std::make_unique<MockGripper>();
}

}  // namespace csrc
