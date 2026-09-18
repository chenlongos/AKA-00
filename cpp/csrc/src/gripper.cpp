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

// ── 没有可用夹爪时的占位（**不是 mock**）──
//
// 与删除前那个 MockGripper 的区别：它从不假装成功 —— 状态恒为 Unknown、每个动作都打
// ERROR，接口/界面看到的就是"夹爪没接上"。MockGripper 的问题是 close() 打完日志就
// 把状态改成 Closed，于是"夹爪没动"和"夹爪动了"在外部完全分不出来。
// 保留一个对象（而不是让工厂返回 nullptr）只是为了调用方不用到处判空。
class UnavailableGripper : public Gripper {
public:
    explicit UnavailableGripper(std::string why) : why_(std::move(why)) {}
    void open() override { CAM_ERROR("[gripper] open 被丢弃：%s", why_.c_str()); }
    void close() override { CAM_ERROR("[gripper] close 被丢弃：%s", why_.c_str()); }
    GripperStatus get_status() override { return GripperStatus::Unknown; }
    void update_angles(const Json&) override {}
    void preview_angle(const std::string& key, int angle) override {
        CAM_ERROR("[gripper] preview_angle(%s=%d) 被丢弃：%s", key.c_str(), angle, why_.c_str());
    }

private:
    std::string why_;
};

// ── ZP10S 适配器 ──
class ZP10SGripperAdapter : public Gripper {
public:
    explicit ZP10SGripperAdapter(std::unique_ptr<ZP10S> zp10s) : zp10s_(std::move(zp10s)) {}

    void open() override {
        if (!zp10s_->ok()) { CAM_ERROR("[gripper] open 被丢弃：zp10s 没连上（%s）", zp10s_->error().c_str()); return; }
        zp10s_release(*zp10s_);
        status_ = GripperStatus::Open;
    }
    void close() override {
        if (!zp10s_->ok()) { CAM_ERROR("[gripper] close 被丢弃：zp10s 没连上（%s）", zp10s_->error().c_str()); return; }
        zp10s_grab(*zp10s_);
        status_ = GripperStatus::Closed;
    }
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
    // 注意：**设备打不开也不再退回 mock**（2026-09-18 删除）—— 那样只会让"夹爪没动"
    // 和"夹爪动了"在外部完全分不出来。打不开就照实报错，让 /api/*/status 里的
    // gripper_status 停在 unknown、每次动作打 ERROR。
    if (driver == "zp10s") {
        auto zp10s = std::make_unique<ZP10S>(port, baudrate);
        if (!zp10s->ok()) {
            CAM_ERROR("[gripper] zp10s 打不开（%s）—— 夹爪不会动", zp10s->error().c_str());
        }
        return std::make_unique<ZP10SGripperAdapter>(std::move(zp10s));
    }
    if (driver == "sts3215") {
        auto servo = std::make_unique<STS3215>(port, baudrate);
        if (!servo->ok()) {
            CAM_ERROR("[gripper] sts3215 打不开（%s）—— 夹爪不会动", servo->error().c_str());
        }
        return std::make_unique<STS3215GripperAdapter>(std::move(servo));
    }
    CAM_ERROR("[gripper] driver=\"%s\" 不支持（mock 已移除，只认 zp10s / sts3215）—— 夹爪不会动",
              driver.c_str());
    return std::make_unique<UnavailableGripper>("driver=\"" + driver + "\" 不支持（mock 已移除）");
}

}  // namespace csrc
