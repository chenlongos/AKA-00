// csrc/zp10s.cpp

#include "csrc/zp10s.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>

#include "csrc/angle_config.hpp"
#include "csrc/log.hpp"

namespace csrc {

ZP10S::ZP10S(const std::string& port, int baudrate) {
    if (!ser_.open(port, baudrate, 0.1)) {
        err_ = ser_.error();
        CAM_WARN("[zp10s] open %s failed: %s", port.c_str(), ser_.error().c_str());
        return;
    }
    load_angles();
    ok_ = true;
    CAM_INFO("[zp10s] port %s opened", port.c_str());
}

void ZP10S::close() { ser_.close(); }

void ZP10S::load_angles() {
    angles_ = load_arm_angles("zp10s");
}

void ZP10S::update_angles(const Json& angles) {
    for (const char* gk : {"grab_position", "lift_position"}) {
        const Json* g = angles.get(gk);
        if (g && g->is_object()) {
            Json& dst = angles_[gk];
            if (!dst.is_object()) dst = Json(Json::Type::Object);
            for (auto& kv : g->object()) {
                dst[kv.first] = kv.second;
            }
        }
    }
    for (const char* sk : {"gripper_open", "gripper_close"}) {
        const Json* v = angles.get(sk);
        if (v) angles_[sk] = *v;
    }
}

int ZP10S::pos(const std::string& group_key, const std::string& servo_key) const {
    const Json* g = angles_.get(group_key);
    if (g && g->is_object()) {
        const Json* v = g->get(servo_key);
        if (v) return (int)v->as_int(150);
    }
    return 150;
}

int ZP10S::gripper_open_angle() const {
    return get_gripper_open(angles_, "zp10s");
}

int ZP10S::gripper_close_angle() const {
    return get_gripper_close(angles_, "zp10s");
}

void ZP10S::send_cmd_str(int servo_id, const std::string& cmd) {
    char buf[32];
    snprintf(buf, sizeof buf, "#%03d%s", servo_id, cmd.c_str());
    ser_.write(buf);
}

void ZP10S::send_frame(int servo_id, double angle) {
    // 角度映射到脉宽 500~2500us
    int pulse = (int)(500 + (angle / 270.0) * 2000);
    if (pulse < 500) pulse = 500;
    if (pulse > 2500) pulse = 2500;
    char buf[32];
    snprintf(buf, sizeof buf, "#%03dP%04dT1000!", servo_id, pulse);
    ser_.write(buf);
    CAM_DEBUG("[zp10s] set_angle servo=%d angle=%.0f → %s", servo_id, angle, buf);
}

bool ZP10S::set_angle(int servo_id, double angle) {
    if (angle < 0 || angle > 270) {
        CAM_WARN("[zp10s] set_angle out of range: %f", angle);
        return false;
    }
    send_frame(servo_id, angle);
    return true;
}

void ZP10S::release_torque() { send_cmd_str(255, "PULK"); }
void ZP10S::restoring_torque() { send_cmd_str(255, "PULR"); }

void ZP10S::send_raw_cmd(const std::string& cmd) {
    if (!cmd.empty()) ser_.write(cmd);
}

void zp10s_grab(ZP10S& servo, const std::string& driver) {
    const int gservo = gripper_servo_id(driver);  // ZP10S = 2
    // 1. 张开夹爪
    servo.set_angle(gservo, servo.gripper_open_angle());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // 2. 夹取位姿（手臂舵机到位，夹爪保持张开）
    Json angles = load_arm_angles(driver);
    for (auto& kv : angles.get("grab_position")->object()) {
        if (kv.first.rfind("servo", 0) == 0) {
            int id = std::atoi(kv.first.c_str() + 5);
            servo.set_angle(id, kv.second.as_int(150));
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    // 3. 闭合夹爪
    servo.set_angle(gservo, servo.gripper_close_angle());
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    // 4. 抬起位姿（手臂舵机抬起，夹爪保持闭合）
    for (auto& kv : angles.get("lift_position")->object()) {
        if (kv.first.rfind("servo", 0) == 0) {
            int id = std::atoi(kv.first.c_str() + 5);
            servo.set_angle(id, kv.second.as_int(150));
        }
    }
    CAM_INFO("[zp10s] grab sequence done");
}

void zp10s_release(ZP10S& servo) {
    servo.set_angle(gripper_servo_id("zp10s"), servo.gripper_open_angle());
    CAM_INFO("[zp10s] release done");
}

}  // namespace csrc
