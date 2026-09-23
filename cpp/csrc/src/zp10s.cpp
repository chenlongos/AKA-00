// csrc/zp10s.cpp

#include "csrc/zp10s.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "csrc/angle_config.hpp"
#include "csrc/log.hpp"

namespace csrc {

namespace {
// ── 闭环夹爪参数 ──
//
// 这几个数只影响"夹住之后怎么收手"，不影响张开/手臂/其它角度。
// 板上实测后按需调（先看日志里那行"夹住：停在 X（目标 Y）"）：
constexpr int kGripPollMs      = 50;    // PRAD 轮询间隔
constexpr int kGripStableCount = 5;     // 连续多少次位置不变算"夹住"（5 × 50ms = 250ms）
constexpr int kGripMinOffset   = 20;    // 距目标至少差这么多脉宽才算"被挡住了"（≈2.7°），
                                        // 用来把"正常走到目标"和"被物体挡住"分开
constexpr int kGripBias        = 20;    // 夹住后保留的位置误差 = 夹持力（大 = 夹得紧、电流大）
constexpr int kGripTimeoutMs   = 1600;  // 等"夹住"的上限；超时就按普通闭合处理
}  // namespace

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

void ZP10S::send_pulse(int servo_id, int pulse, int time_ms) {
    if (pulse < 500) pulse = 500;
    if (pulse > 2500) pulse = 2500;
    if (time_ms < 0) time_ms = 0;
    if (time_ms > 9999) time_ms = 9999;
    char buf[32];
    snprintf(buf, sizeof buf, "#%03dP%04dT%04d!", servo_id, pulse, time_ms);
    ser_.write(buf);
    CAM_DEBUG("[zp10s] servo=%d pulse=%d t=%dms → %s", servo_id, pulse, time_ms, buf);
}

void ZP10S::send_frame(int servo_id, double angle) {
    // 角度映射到脉宽 500~2500us
    send_pulse(servo_id, (int)(500 + (angle / 270.0) * 2000), 1000);
}

bool ZP10S::read_position(int servo_id, int& pulse) {
    char cmd[16];
    snprintf(cmd, sizeof cmd, "#%03dPRAD!", servo_id);
    ser_.clear_input();   // 丢掉上一次残留，否则会把旧回包当成这一次的
    if (!ser_.write(cmd)) return false;

    // 回包形如 "#000P1500!"（手册 p.26 第 9 条）
    char buf[32];
    size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (got < sizeof(buf) - 1 && std::chrono::steady_clock::now() < deadline) {
        size_t n = ser_.read((uint8_t*)buf + got, sizeof(buf) - 1 - got);
        if (n == 0) continue;               // 单次读超时（0.1s）—— 还没到 deadline 就再等
        got += n;
        buf[got] = '\0';
        if (std::strchr(buf, '!')) break;   // 帧尾到了
    }
    buf[got] = '\0';

    const char* hash = std::strchr(buf, '#');
    const char* p = hash ? std::strchr(hash, 'P') : nullptr;
    if (!p) {
        CAM_DEBUG("[zp10s] read_position servo=%d 无回包/无 P 段: %s", servo_id, buf);
        return false;
    }
    int v = std::atoi(p + 1);
    // 挡掉 "#000P!"（ID 检测）、"#000PMOD1!"（模式）这类回包：它们的 P 后面不是位置
    if (v < 500 || v > 2500) {
        CAM_DEBUG("[zp10s] read_position servo=%d 回包不是位置: %s", servo_id, buf);
        return false;
    }
    pulse = v;
    return true;
}

int ZP10S::grip_until_stall(int servo_id, double angle, int bias, int timeout_ms) {
    const int target = (int)(500 + (angle / 270.0) * 2000);
    send_pulse(servo_id, target, 1000);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    int last = -1;
    int stable = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kGripPollMs));

        int pos = 0;
        if (!read_position(servo_id, pos)) continue;   // 这一拍没读到，不算"不动"

        if (last >= 0 && std::abs(pos - last) <= 2) stable++;
        else                                        stable = 0;
        last = pos;

        // 一直没动 + 又明显没走到目标 —— 两者同时成立才算被物体挡住了
        if (stable < kGripStableCount || std::abs(pos - target) < kGripMinOffset) continue;

        const int dir = (target < pos) ? -1 : 1;       // 往"更闭合"的方向
        const int hold = pos + dir * bias;
        send_pulse(servo_id, hold, 200);               // 收手：停在夹住处，只留 bias 的预紧
        CAM_INFO("[zp10s] 夹住：停在 %d（目标 %d），改持 %d（bias %d，误差即夹持力）",
                 pos, target, hold, bias);
        return pos;
    }

    // 超时：没夹到东西（空合，正常走到了目标），或这颗固件不回 PRAD
    CAM_DEBUG("[zp10s] grip_until_stall servo=%d 超时（%dms），按普通闭合处理",
              servo_id, timeout_ms);
    return -1;
}

bool ZP10S::set_angle(int servo_id, double angle) {
    if (angle < 0 || angle > 270) {
        CAM_WARN("[zp10s] set_angle out of range: %f", angle);
        return false;
    }
    send_frame(servo_id, angle);
    return true;
}

// 手册 p.25 明写 "#" 和 "!" 是固定英文格式 —— 少了 "!" 就是残帧，控制板不会执行。
void ZP10S::release_torque() { send_cmd_str(255, "PULK!"); }
void ZP10S::restoring_torque() { send_cmd_str(255, "PULR!"); }

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
    // 3. 闭合夹爪 —— 闭环：夹住就收手，不再持续硬顶（见 grip_until_stall 注释）
    //    这一步本身就阻塞到"夹住"或超时，所以后面不用再干等 2 秒。
    const int stalled = servo.grip_until_stall(gservo, servo.gripper_close_angle(),
                                               kGripBias, kGripTimeoutMs);
    if (stalled < 0) CAM_INFO("[zp10s] 夹爪闭合：没读到夹住（空合，或固件不回 PRAD）");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
