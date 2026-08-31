// csrc/tt_pid.cpp

#include "csrc/tt_pid.hpp"

#include <cstring>
#include <thread>

#include "csrc/log.hpp"

namespace csrc {

TtPidChassis::TtPidChassis(const std::string& port, int baudrate, int ppr, int pwm_freq)
    : ppr_(ppr), pwm_freq_(pwm_freq) {
    if (!ser_.open(port, baudrate, 0.1)) {
        err_ = "open " + port + " failed: " + ser_.error();
        CAM_WARN("[tt_pid] %s", err_.c_str());
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // 等 ESP32 枚举
    ser_.clear_input();

    if (!init_chassis()) {
        err_ = "ESP32 init failed";
        CAM_WARN("[tt_pid] %s", err_.c_str());
        return;
    }
    if (!config(ppr, pwm_freq)) {
        err_ = "ESP32 config failed";
        CAM_WARN("[tt_pid] %s", err_.c_str());
        return;
    }
    ok_ = true;
    CAM_INFO("[tt_pid] ✓ port opened, ppr=%d pwm_freq=%d", ppr, pwm_freq);
}

TtPidChassis::~TtPidChassis() { close(); }

void TtPidChassis::close() { ser_.close(); }

bool TtPidChassis::init_chassis() {
    int rsp = send_cmd(CMD_INIT, nullptr, 0, nullptr, nullptr);
    return rsp == RSP_ACK;
}

bool TtPidChassis::config(int ppr, int pwm_freq) {
    // struct.pack(">HH", ppr, pwm_freq) — 2 个大端 u16
    uint8_t payload[4] = {
        (uint8_t)(ppr >> 8), (uint8_t)ppr,
        (uint8_t)(pwm_freq >> 8), (uint8_t)pwm_freq,
    };
    int rsp = send_cmd(CMD_CONFIG, payload, sizeof payload, nullptr, nullptr);
    return rsp == RSP_ACK;
}

void TtPidChassis::send_cmd_noresp(uint8_t cmd, const uint8_t* payload, size_t len) {
    if (!ok_) return;
    std::lock_guard<std::mutex> lk(io_mu_);
    uint8_t chk = cmd ^ (uint8_t)len;
    for (size_t i = 0; i < len; i++) chk ^= payload[i];

    uint8_t frame[260];
    frame[0] = FRAME_H1;
    frame[1] = FRAME_H2;
    frame[2] = cmd;
    frame[3] = (uint8_t)len;
    if (len) std::memcpy(frame + 4, payload, len);
    frame[4 + len] = chk;

    ser_.clear_input();  // 发前清输入缓冲，防残留干扰
    if (!ser_.write(frame, 4 + len + 1)) {
        CAM_WARN("[tt_pid] tx 0x%02X write failed", cmd);
    }
    if (csrc::log_level() >= csrc::LogLevel::Debug) {
        char hex[64];
        size_t n = 4 + len + 1;
        for (size_t i = 0; i < n && i * 3 + 2 < sizeof hex; i++) {
            snprintf(hex + i * 3, sizeof hex - i * 3, "%02X ", frame[i]);
        }
        CAM_DEBUG("[tt_pid/tx] 0x%02X len=%zu -> %s", cmd, len, hex);
    }
}

bool TtPidChassis::recv_frame(uint8_t* rsp, uint8_t* payload, size_t* payload_len, double timeout) {
    uint8_t header[4];
    if (!ser_.read_exact(header, 4)) {
        CAM_DEBUG("[tt_pid/rx] header timeout");
        return false;
    }
    if (header[0] != FRAME_H1 || header[1] != FRAME_H2) {
        CAM_DEBUG("[tt_pid/rx] bad header %02X %02X", header[0], header[1]);
        return false;
    }
    uint8_t cmd = header[2];
    uint8_t len = header[3];

    uint8_t tmp[256];
    if (len && !ser_.read_exact(tmp, len)) return false;
    if (payload && payload_len && len <= *payload_len) {
        std::memcpy(payload, tmp, len);
        *payload_len = len;
    } else if (payload_len) {
        *payload_len = len;
    }

    uint8_t chk_b;
    if (!ser_.read_exact(&chk_b, 1)) return false;
    uint8_t expected = cmd ^ len;
    for (size_t i = 0; i < len; i++) expected ^= tmp[i];
    if (expected != chk_b) {
        CAM_DEBUG("[tt_pid/rx] chk mismatch");
        return false;
    }
    if (rsp) *rsp = cmd;
    return true;
}

int TtPidChassis::send_cmd(uint8_t cmd, const uint8_t* payload, size_t len,
                           uint8_t* rsp_payload, size_t* rsp_len, double timeout) {
    if (!ok_ && cmd != CMD_INIT && cmd != CMD_CONFIG) return -1;
    std::lock_guard<std::mutex> lk(io_mu_);
    ser_.clear_input();
    uint8_t chk = cmd ^ (uint8_t)len;
    for (size_t i = 0; i < len; i++) chk ^= payload[i];
    uint8_t frame[260];
    frame[0] = FRAME_H1;
    frame[1] = FRAME_H2;
    frame[2] = cmd;
    frame[3] = (uint8_t)len;
    if (len) std::memcpy(frame + 4, payload, len);
    frame[4 + len] = chk;
    if (!ser_.write(frame, 4 + len + 1)) return -1;

    uint8_t rsp = 0;
    size_t got = 0;
    if (rsp_payload && rsp_len) got = *rsp_len;
    if (!recv_frame(&rsp, rsp_payload, &got, timeout)) return -1;
    if (rsp_len) *rsp_len = got;
    return rsp;
}

void TtPidChassis::set_speed(int left, int right) {
    int l = left < -100 ? -100 : (left > 100 ? 100 : left);
    int r = right < -100 ? -100 : (right > 100 ? 100 : right);
    // struct.pack(">hh", left, right) — 2 个大端 i16
    uint8_t payload[4] = {
        (uint8_t)(l >> 8), (uint8_t)l,
        (uint8_t)(r >> 8), (uint8_t)r,
    };
    send_cmd_noresp(CMD_SET_SPEEDS, payload, 4);
}

void TtPidChassis::brake() {
    send_cmd_noresp(CMD_BRAKE, (const uint8_t*)"\x02", 1);
}

void TtPidChassis::sleep() {
    send_cmd_noresp(CMD_STOP, (const uint8_t*)"\x02", 1);
}

bool TtPidChassis::reinitialize() {
    if (!ok_) return false;
    bool a = init_chassis();
    bool b = config(ppr_, pwm_freq_);
    return a && b;
}

void TtPidChassis::get_speeds(int& left_rpm, int& right_rpm) {
    left_rpm = right_rpm = 0;
    if (!ok_) return;
    std::lock_guard<std::mutex> lk(io_mu_);
    // ESP32 固件 GET_STATUS(0x21) 响应: [status, m1_hi, m1_lo, m2_hi, m2_lo]
    ser_.clear_input();
    uint8_t chk = CMD_GET_STATUS ^ 0;
    uint8_t frame[5] = {FRAME_H1, FRAME_H2, CMD_GET_STATUS, 0, chk};
    if (!ser_.write(frame, sizeof frame)) return;

    uint8_t payload[5];
    size_t plen = sizeof payload;
    uint8_t rsp = 0;
    if (!recv_frame(&rsp, payload, &plen, 0.1) || rsp != RSP_STATUS || plen < 5) {
        CAM_DEBUG("[tt_pid] GET_STATUS failed (rsp=0x%02X len=%zu)", rsp, plen);
        return;
    }
    left_rpm = (int)(int16_t)(((uint16_t)payload[1] << 8) | payload[2]);
    right_rpm = (int)(int16_t)(((uint16_t)payload[3] << 8) | payload[4]);
}

void TtPidChassis::get_encoder(int& c1, int& c2) {
    c1 = c2 = 0;
    if (!ok_) return;
    uint8_t payload[8];
    size_t plen = sizeof payload;
    int rsp = send_cmd(CMD_GET_ENCODER, nullptr, 0, payload, &plen);
    (void)rsp;
    if (plen >= 8) {
        c1 = (int)(int32_t)(((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                            ((uint32_t)payload[2] << 8) | payload[3]);
        c2 = (int)(int32_t)(((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
                            ((uint32_t)payload[6] << 8) | payload[7]);
    }
}

void TtPidChassis::move_distance(uint8_t dir, uint8_t speed, int32_t target) {
    // struct.pack(">BBi", d, speed, target) — dir(1) speed(1) target(4B 大端)
    uint8_t payload[6] = {
        dir, speed,
        (uint8_t)(target >> 24), (uint8_t)(target >> 16),
        (uint8_t)(target >> 8), (uint8_t)target,
    };
    send_cmd_noresp(CMD_MOVE_DISTANCE, payload, 6);
    CAM_INFO("[tt_pid] CMD_MOVE_DISTANCE dir=%u speed=%u target=%d", dir, speed, target);
}

}  // namespace csrc
