// capp/websocket.cpp

#include "capp/websocket.hpp"

#include <chrono>
#include <cstring>

#include "capp/context.hpp"
#include "csrc/base64.hpp"
#include "csrc/log.hpp"
#include "csrc/sha1.hpp"
#include "csrc/system_utils.hpp"

namespace capp {

namespace {
constexpr char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
}

bool ws_handshake(const HttpRequest& req, ClientConn& conn) {
    std::string key = req.header("sec-websocket-key");
    if (key.empty()) return false;

    std::string accept = csrc::base64_encode(csrc::sha1(key + kWsGuid));

    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    return conn.write_all(resp);
}

bool ws_send_frame(ClientConn& conn, uint8_t opcode, const void* data, size_t len) {
    uint8_t header[14];
    size_t n = 0;
    header[n++] = (uint8_t)(0x80 | (opcode & 0x0F));  // FIN + opcode
    if (len < 126) {
        header[n++] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        header[n++] = 126;
        header[n++] = (uint8_t)(len >> 8);
        header[n++] = (uint8_t)len;
    } else {
        header[n++] = 127;
        uint64_t l = len;
        for (int i = 7; i >= 0; i--) header[n++] = (uint8_t)(l >> (i * 8));
    }
    if (!conn.write_all(header, n)) return false;
    if (len && !conn.write_all(data, len)) return false;
    return true;
}

bool ws_send_binary(ClientConn& conn, const void* data, size_t len) {
    return ws_send_frame(conn, 0x2, data, len);
}

/// 读取一帧。返回:
///   >0  opcode（payload 写入 out）
///    0  超时（无数据，连接仍可用）
///   -1  连接关闭 / 协议错误（应断开）
///   -2  ping/pong（已处理，忽略）
int ws_read_frame(ClientConn& conn, uint8_t* out, size_t cap, size_t& out_len, int timeout_ms) {
    out_len = 0;
    uint8_t hdr[2];
    // 帧头必须循环读满 2 字节：TCP 分片时（WiFi 常见）单次 recv 可能只到 1 字节，
    // 之前误判为协议错误直接断开 → 前端摇杆命令丢失、连接反复重连。
    size_t got = 0;
    while (got < 2) {
        size_t g = 0;
        int r = conn.read_some(hdr + got, 2 - got, timeout_ms, g);
        if (r == -1) return -1;              // 关闭/错误
        if (r == 0) return got == 0 ? 0 : -1;  // 无数据：正常节拍；读到一半超时 = 坏帧
        if (g == 0) return -1;
        got += g;
    }

    bool fin = (hdr[0] & 0x80) != 0;
    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;

    auto read_exact_short = [&](uint8_t* b, size_t n, int timeout) -> bool {
        size_t total = 0;
        while (total < n) {
            size_t g = 0;
            int r = conn.read_some(b + total, n - total, timeout, g);
            if (r <= 0 || g == 0) return false;
            total += g;
        }
        return true;
    };

    if (len == 126) {
        uint8_t b[2];
        if (!read_exact_short(b, 2, timeout_ms * 5)) return -1;
        len = ((uint64_t)b[0] << 8) | b[1];
    } else if (len == 127) {
        uint8_t b[8];
        if (!read_exact_short(b, 8, timeout_ms * 5)) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | b[i];
    }
    if (len > cap || len > (1 << 20)) return -1;  // 帧过大

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (!read_exact_short(mask, 4, timeout_ms * 5)) return -1;
    }

    // 读 payload（分块；帧内超时放宽，防 WiFi 弱时 TCP 分片被误判断开）
    const int frame_timeout = timeout_ms * 5;
    size_t read_total = 0;
    while (read_total < len) {
        size_t chunk = (size_t)len - read_total;
        size_t g = 0;
        int r = conn.read_some(out + read_total, chunk > 4096 ? 4096 : chunk, frame_timeout, g);
        if (r <= 0 || g == 0) return -1;
        read_total += g;
    }
    if (masked) {
        for (uint64_t i = 0; i < len; i++) out[i] ^= mask[i % 4];
    }
    out_len = (size_t)len;

    if (opcode == 0x8) return -1;  // close
    if (opcode == 0x9) {           // ping → pong
        ws_send_frame(conn, 0xA, out, out_len);
        return -2;
    }
    if (opcode == 0xA) return -2;  // pong
    if (!fin) return -1;           // 不支持分片
    return opcode;
}

// ═══════════════════════ /ws/control ═══════════════════════

/// joystick → tank（差速转向：左 = y+x，右 = y-x，×100 映射到 ±100 限幅）
/// 注意：必须先乘 100 再取整 —— 之前漏了 ×100，fy+fx∈(-2,2) 直接 (int) 截断成 0，
/// 导致摇杆速度恒为 0、小车不动。
static void joystick_to_tank(int x, int y, int& left, int& right) {
    double fy = y / 127.0;
    double fx = x / 127.0;
    auto c = [](double v) -> int {
        if (v > 100.0) v = 100.0;
        if (v < -100.0) v = -100.0;
        return (int)v;
    };
    left = c((fy + fx) * 100.0);
    right = c((fy - fx) * 100.0);
}

static bool ws_send_json(ClientConn& conn, const csrc::Json& j) {
    std::string json = j.dump(false);
    std::string buf;
    buf.push_back((char)0xDD);
    buf += json;
    return ws_send_binary(conn, buf.data(), buf.size());
}

static void ws_handle_json(AppContext& ctx, ClientConn& conn, const csrc::Json& cmd) {
    std::string type = cmd.gets("type");
    if (type == "ip") {
        csrc::Json resp;
        resp["type"] = "ip";
        resp["ip"] = csrc::detect_local_ip();
        ws_send_json(conn, resp);
        return;
    }
    if (type == "action") {
        std::string action = cmd.gets("action", "stop");
        int speed = (int)cmd.geti("speed", 50);
        double ms = (double)cmd.geti("time", 0);
        csrc::Json result = execute_action(ctx, action, speed, ms);
        csrc::Json resp;
        resp["type"] = "action";
        resp["result"] = result;
        ws_send_json(conn, resp);
        return;
    }
    if (type == "raw_command") {
        csrc::Json result = send_raw_command(ctx, cmd.gets("cmd"));
        csrc::Json resp;
        resp["type"] = "raw_command";
        resp["result"] = result;
        ws_send_json(conn, resp);
        return;
    }
    if (type == "reinitialize") {
        csrc::Json result = reinitialize_motor_pair(ctx);
        csrc::Json resp;
        resp["type"] = "reinitialize";
        resp["result"] = result;
        ws_send_json(conn, resp);
        return;
    }
    if (type == "arm_cmd") {
        const csrc::Json* payload = cmd.get("payload");
        if (payload && payload->is_object()) {
            std::string command = payload->gets("command");
            if (command == "grab" || command == "release") {
                execute_action(ctx, command, 50, 0);
            }
        }
        return;
    }
    CAM_DEBUG("[ws] ignore type=%s (use HTTP for this)", type.c_str());
}

void ws_control_loop(AppContext& ctx, ClientConn& conn) {
    CAM_INFO("[ws] client connected");

    // 开场白（触发前端 wsReady 状态机）
    csrc::Json welcome;
    welcome["type"] = "ip";
    welcome["ip"] = csrc::detect_local_ip();
    ws_send_json(conn, welcome);

    auto last_status = std::chrono::steady_clock::now();
    bool running = true;

    while (running) {
        // 200ms 节拍：读帧（超时继续）或推状态
        uint8_t payload[4096];
        size_t len = 0;
        int opcode = ws_read_frame(conn, payload, sizeof payload, len, 200);

        if (opcode == -1) {   // 关闭 / 协议错误
            CAM_DEBUG("[ws] connection closed (peer close or protocol error)");
            running = false;
            break;
        }
        if (opcode == 0) {    // 超时：不处理，下面照常推状态
        } else if (opcode == 0x2 && len >= 2) {  // binary
            if (payload[0] == 0xAA && len >= 3) {
                int x = (int8_t)payload[1];
                int y = (int8_t)payload[2];
                int left, right;
                joystick_to_tank(x, y, left, right);
                CAM_DEBUG("[ws] joystick x=%d y=%d -> L=%d R=%d", x, y, left, right);
                csrc::Json cmd;
                cmd["type"] = "joystick";
                cmd["x"] = csrc::Json((int64_t)x);
                cmd["y"] = csrc::Json((int64_t)y);
                ctx.log_command(cmd);
                run_motor(ctx, left, right, 0);
            } else if (payload[0] == 0xDD && len >= 2) {
                std::string json_str((const char*)payload + 1, len - 1);
                csrc::Json cmd = csrc::Json::parse_or(json_str);
                if (cmd.is_object()) ws_handle_json(ctx, conn, cmd);
            }
        } else if (opcode == 0x1 && len > 0) {  // text（宽松兼容）
            std::string json_str((const char*)payload, len);
            csrc::Json cmd = csrc::Json::parse_or(json_str);
            if (cmd.is_object()) ws_handle_json(ctx, conn, cmd);
        }
        // -2 (ping/pong) 忽略

        // 每 200ms 推电机状态 0xBB left right (m/s × 1000, int16 LE)
        auto now = std::chrono::steady_clock::now();
        if (now - last_status >= std::chrono::milliseconds(200)) {
            csrc::RobotStatus s = ctx.collector.get_status();
            int16_t left_mmps = (int16_t)(s.left_speed * 1000.0 + 0.5);
            int16_t right_mmps = (int16_t)(s.right_speed * 1000.0 + 0.5);
            uint8_t buf[5];
            buf[0] = 0xBB;
            buf[1] = (uint8_t)(left_mmps & 0xFF);
            buf[2] = (uint8_t)((left_mmps >> 8) & 0xFF);
            buf[3] = (uint8_t)(right_mmps & 0xFF);
            buf[4] = (uint8_t)((right_mmps >> 8) & 0xFF);
            if (!ws_send_binary(conn, buf, 5)) {
                running = false;
                break;
            }
            last_status = now;
        }
    }

    // 断开自动停电机（摇杆松手不跑车）
    run_motor(ctx, 0, 0, 0);
    conn.close();
    CAM_INFO("[ws] client disconnected");
}

}  // namespace capp
