// csrc/sts3215.cpp

#include "csrc/sts3215.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "csrc/angle_config.hpp"
#include "csrc/log.hpp"

namespace csrc {

// 寄存器地址
inline constexpr uint8_t REG_POS_TARGET = 0x2A;
inline constexpr uint8_t REG_POS_CURRENT = 0x38;
inline constexpr uint8_t REG_SPEED = 0x2E;
inline constexpr uint8_t REG_MAX_TORQUE = 0x10;
inline constexpr uint8_t REG_PROTECT_CURRENT = 0x40;
inline constexpr uint8_t REG_OVERLOAD_TORQUE = 0x24;
inline constexpr uint8_t REG_OPERATING_MODE = 0x21;
inline constexpr uint8_t REG_P_COEFF = 0x15;
inline constexpr uint8_t REG_I_COEFF = 0x17;
inline constexpr uint8_t REG_D_COEFF = 0x16;

inline constexpr uint8_t INST_READ = 0x02;
inline constexpr uint8_t INST_WRITE = 0x03;

STS3215::STS3215(const std::string& port, int baudrate) {
    if (!ser_.open(port, baudrate, 0.1)) {
        err_ = ser_.error();
        CAM_WARN("[sts3215] open %s failed: %s", port.c_str(), ser_.error().c_str());
        return;
    }
    angles_ = load_arm_angles("sts3215");
    ok_ = true;
    CAM_INFO("[sts3215] port %s opened", port.c_str());
}

void STS3215::close() { ser_.close(); }

void STS3215::update_angles(const Json& angles) {
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

int STS3215::pos(const std::string& group_key, const std::string& servo_key) const {
    const Json* g = angles_.get(group_key);
    if (g && g->is_object()) {
        const Json* v = g->get(servo_key);
        if (v) return (int)v->as_int(4000);
    }
    return 4000;
}

int STS3215::gripper_open_angle() const {
    return get_gripper_open(angles_, "sts3215");
}

int STS3215::gripper_close_angle() const {
    return get_gripper_close(angles_, "sts3215");
}

uint8_t STS3215::checksum(const uint8_t* data, size_t len) const {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return (uint8_t)(~sum);
}

void STS3215::send_cmd(uint8_t servo_id, uint8_t instruction, const uint8_t* params, size_t len) {
    uint8_t pkt[64];
    size_t n = 0;
    pkt[n++] = 0xFF;
    pkt[n++] = 0xFF;
    pkt[n++] = servo_id;
    pkt[n++] = (uint8_t)(len + 2);  // length = params + inst + chk
    pkt[n++] = instruction;
    if (len) std::memcpy(pkt + n, params, len);
    n += len;
    pkt[n] = checksum(pkt + 2, n - 2);
    n += 1;

    ser_.clear_input();
    ser_.write(pkt, n);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // Python time.sleep(0.005)
    CAM_DEBUG("[sts3215] tx id=%u inst=0x%02X len=%zu", servo_id, instruction, len);
}

void STS3215::write_reg(uint8_t servo_id, uint8_t addr, const uint8_t* data, size_t len) {
    uint8_t params[16];
    params[0] = addr;
    std::memcpy(params + 1, data, len);
    send_cmd(servo_id, INST_WRITE, params, 1 + len);
}

bool STS3215::read_data(uint8_t servo_id, uint8_t addr, size_t length, uint8_t* out, size_t* out_len) {
    uint8_t params[2] = {addr, (uint8_t)length};
    send_cmd(servo_id, INST_READ, params, 2);

    // 等响应: FF FF <id> <len> 00 <data...> <chk>
    size_t total = 6 + length;
    uint8_t buf[64];
    size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (got < total && std::chrono::steady_clock::now() < deadline) {
        size_t n = ser_.read(buf + got, total - got);
        got += n;
    }
    if (got < 6) {
        CAM_DEBUG("[sts3215] read_data timeout (got %zu/%zu)", got, total);
        return false;
    }
    if (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == servo_id && buf[4] == 0x00) {
        if (got >= 5 + length) {
            std::memcpy(out, buf + 5, length);
            if (out_len) *out_len = length;
            return true;
        }
    }
    return false;
}

void STS3215::move_to_position(uint8_t servo_id, int pos) {
    if (pos < 0) pos = 0;
    if (pos > 4095) pos = 4095;
    uint8_t data[2] = {(uint8_t)(pos & 0xFF), (uint8_t)((pos >> 8) & 0xFF)};  // LE
    write_reg(servo_id, REG_POS_TARGET, data, 2);
}

bool STS3215::get_position(uint8_t servo_id, int& pos) {
    uint8_t data[2];
    size_t n = 0;
    if (!read_data(servo_id, REG_POS_CURRENT, 2, data, &n) || n < 2) return false;
    pos = data[0] | (data[1] << 8);  // LE
    return true;
}

void STS3215::move_angle(uint8_t servo_id, double angle) {
    move_to_position(servo_id, (int)((angle / 360.0) * 4095));
}

void STS3215::set_speed(uint8_t servo_id, int speed) {
    uint8_t data[2] = {(uint8_t)(speed & 0xFF), (uint8_t)((speed >> 8) & 0xFF)};
    write_reg(servo_id, REG_SPEED, data, 2);
}

void STS3215::set_max_torque_limit(uint8_t servo_id, int torque) {
    uint8_t data[2] = {(uint8_t)(torque & 0xFF), (uint8_t)((torque >> 8) & 0xFF)};
    write_reg(servo_id, REG_MAX_TORQUE, data, 2);
}

void STS3215::set_protection_current(uint8_t servo_id, int torque) {
    uint8_t data[2] = {(uint8_t)(torque & 0xFF), (uint8_t)((torque >> 8) & 0xFF)};
    write_reg(servo_id, REG_PROTECT_CURRENT, data, 2);
}

void STS3215::set_overload_torque(uint8_t servo_id, int torque) {
    uint8_t data[1] = {(uint8_t)(torque & 0xFF)};
    write_reg(servo_id, REG_OVERLOAD_TORQUE, data, 1);
}

void STS3215::set_operating_mode(uint8_t servo_id, int mode) {
    uint8_t data[1] = {(uint8_t)(mode & 0xFF)};
    write_reg(servo_id, REG_OPERATING_MODE, data, 1);
}

void STS3215::set_p_coefficient(uint8_t servo_id, int v) {
    uint8_t data[1] = {(uint8_t)(v & 0xFF)};
    write_reg(servo_id, REG_P_COEFF, data, 1);
}

void STS3215::set_i_coefficient(uint8_t servo_id, int v) {
    uint8_t data[1] = {(uint8_t)(v & 0xFF)};
    write_reg(servo_id, REG_I_COEFF, data, 1);
}

void STS3215::set_d_coefficient(uint8_t servo_id, int v) {
    uint8_t data[1] = {(uint8_t)(v & 0xFF)};
    write_reg(servo_id, REG_D_COEFF, data, 1);
}

// ── 高层动作（对齐 Python）──

void sts3215_arm_init(STS3215& servo) {
    for (int i = 1; i <= 3; i++) {
        servo.set_operating_mode((uint8_t)i, 0);
        servo.set_speed((uint8_t)i, 1500);
        servo.set_p_coefficient((uint8_t)i, 16);
        servo.set_i_coefficient((uint8_t)i, 0);
        servo.set_d_coefficient((uint8_t)i, 32);
        if (i == 3 || i == 1) {
            servo.set_max_torque_limit((uint8_t)i, 500);
            servo.set_protection_current((uint8_t)i, 250);
            servo.set_overload_torque((uint8_t)i, 25);
        }
    }
}

void sts3215_grab(STS3215& servo) {
    const int gservo = gripper_servo_id("sts3215");  // 3
    // 1. 张开夹爪 + 抬起位姿作为中间安全位姿
    servo.move_to_position((uint8_t)gservo, servo.gripper_open_angle());
    servo.move_to_position(2, servo.pos("lift_position", "servo2"));
    servo.move_to_position(1, servo.pos("lift_position", "servo1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    // 2. 夹取位姿
    servo.move_to_position(1, servo.pos("grab_position", "servo1"));
    servo.move_to_position(2, servo.pos("grab_position", "servo2"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    // 3. 闭合夹爪
    servo.move_to_position((uint8_t)gservo, servo.gripper_close_angle());
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    // 4. 抬起
    servo.move_to_position(1, servo.pos("lift_position", "servo1"));
    servo.move_to_position(2, servo.pos("lift_position", "servo2"));
    CAM_INFO("[sts3215] grab sequence done");
}

void sts3215_release(STS3215& servo) {
    servo.move_to_position(1, servo.pos("lift_position", "servo1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    servo.move_to_position((uint8_t)gripper_servo_id("sts3215"), servo.gripper_open_angle());
    CAM_INFO("[sts3215] release done");
}

}  // namespace csrc
