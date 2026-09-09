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
// ── 帧级超时：同一帧从首字节起超过该时长视为坏帧/死链 ──
constexpr int64_t kFrameTimeoutMs = 5000;

namespace {
struct WsFrame {
    bool fin = true;
    int opcode = 0;
    bool masked = false;
    uint8_t mask[4] = {0, 0, 0, 0};
    size_t payload_off = 0;
    size_t payload_len = 0;
};
enum class Parse { NeedMore, Ok, Error };
/// 从 peer.buf 头部解析一个完整帧（字节不够返回 NeedMore，不视为错误）
Parse ws_parse_frame(const std::vector<uint8_t>& buf, WsFrame& f) {
    if (buf.size() < 2) return Parse::NeedMore;
    f.fin = (buf[0] & 0x80) != 0;
    f.opcode = buf[0] & 0x0F;
    f.masked = (buf[1] & 0x80) != 0;
    uint64_t len7 = buf[1] & 0x7F;
    size_t off = 2;
    uint64_t plen;
    if (len7 == 126) {
        if (buf.size() < off + 2) return Parse::NeedMore;
        plen = ((uint64_t)buf[off] << 8) | buf[off + 1];
        off += 2;
    } else if (len7 == 127) {
        if (buf.size() < off + 8) return Parse::NeedMore;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | buf[off + i];
        off += 8;
    } else {
        plen = len7;
    }
    if (plen > (1u << 20)) return Parse::Error;  // 长度字段超限
    if (f.masked) {
        if (buf.size() < off + 4) return Parse::NeedMore;
        std::memcpy(f.mask, buf.data() + off, 4);
        off += 4;
    }
    if (buf.size() < off + plen) return Parse::NeedMore;
    f.payload_off = off;
    f.payload_len = (size_t)plen;
    return Parse::Ok;
}
const char* ws_reason_str(WsCloseReason r) {
    switch (r) {
        case WsCloseReason::ClientClose:  return "client-close";
        case WsCloseReason::ReadError:    return "read-error";
        case WsCloseReason::Oversize:     return "oversize";
        case WsCloseReason::Fragmented:   return "fragmented";
        case WsCloseReason::FrameTimeout: return "frame-timeout";
        case WsCloseReason::PingTimeout:  return "ping-timeout";
        case WsCloseReason::WriteFail:    return "write-fail";
        default:                          return "none";
    }
}
}  // namespace

/// 有状态读帧：字节跨节拍缓存在 peer.buf 里累积，任何"半帧恰好超时"都不会断开；
/// 只有整帧超过 kFrameTimeoutMs / 协议错误 / 底层读错误才返回 -1（reason 说明）。
int ws_rx_frame(ClientConn& conn, WsPeer& peer, uint8_t* out, size_t cap,
                size_t& out_len, int timeout_ms, WsCloseReason& reason,
                bool& got_any_byte) {
    out_len = 0;
    reason = WsCloseReason::None;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true) {
        // 1) 先尽量从缓冲区解析出完整帧（可能含多个帧：ping/pong/数据连发）
        WsFrame f;
        Parse p = ws_parse_frame(peer.buf, f);
        if (p == Parse::Ok) {
            size_t consumed = f.payload_off + f.payload_len;
            if (f.opcode == 0x8) {  // close
                reason = WsCloseReason::ClientClose;
                return -1;
            }
            if (!f.fin) {  // 分片（本项目不支持）
                reason = WsCloseReason::Fragmented;
                return -1;
            }
            if (f.payload_len > cap) {  // 超出接收缓冲
                reason = WsCloseReason::Oversize;
                return -1;
            }
            if (f.payload_len) {
                const uint8_t* src = peer.buf.data() + f.payload_off;
                if (f.masked) {
                    for (size_t i = 0; i < f.payload_len; i++)
                        out[i] = src[i] ^ f.mask[i % 4];
                } else {
                    std::memcpy(out, src, f.payload_len);
                }
            }
            peer.buf.erase(peer.buf.begin(), peer.buf.begin() + (long)consumed);
            got_any_byte = true;
            if (f.opcode == 0x9) {  // ping → 回 pong，继续处理
                ws_send_frame(conn, 0xA, out, f.payload_len);
                continue;
            }
            if (f.opcode == 0xA) continue;  // pong：活跃信号已由 got_any_byte 记录
            out_len = f.payload_len;
            return f.opcode;  // 0x1 / 0x2 数据帧
        }
        if (p == Parse::Error) {
            reason = WsCloseReason::Oversize;
            return -1;
        }
        // NeedMore：继续收字节
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;  // 本拍无完整帧 → 返回 0（正常节拍）
        uint8_t tmp[256];
        size_t g = 0;
        int remain = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - now).count();
        if (remain < 1) remain = 1;
        int r = conn.read_some(tmp, sizeof tmp, remain, g);
        if (r == -1) { reason = WsCloseReason::ReadError; return -1; }
        if (r == 0 || g == 0) continue;
        got_any_byte = true;
        peer.buf.insert(peer.buf.end(), tmp, tmp + g);
        if (!peer.frame_started) {
            peer.frame_started = true;
            peer.frame_t0 = std::chrono::steady_clock::now();
        }
        // 帧级超时：同一帧首字节之后超过 5s 没拼完整 → 坏帧断开（防半帧死等）
        int64_t age = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - peer.frame_t0).count();
        if (age >= kFrameTimeoutMs) {
            reason = WsCloseReason::FrameTimeout;
            return -1;
        }
    }
    return 0;
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
        CAM_INFO("[ws] action=%s speed=%d time=%.0fms", action.c_str(), speed, ms);
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

    // 底盘连接状态推送：建连即发一次，此后仅在 connected/state 变化时推送，
    // 前端无需轮询/刷新即可感知"底盘已连接 / 未连接自动重连中"。
    std::string last_motor_sig;
    auto send_motor_status = [&](bool force) {
        csrc::Json st = motor_status_json(ctx);
        std::string sig = st.gets("state") + "|" +
                          std::to_string((long long)st.geti("connected"));
        if (!force && sig == last_motor_sig) return;
        last_motor_sig = sig;
        csrc::Json msg;
        msg["type"] = "motor_status";
        msg["motor"] = st;
        ws_send_json(conn, msg);
    };
    send_motor_status(true);

    auto last_status = std::chrono::steady_clock::now();
    auto last_rx_any = std::chrono::steady_clock::now();  // 最近收到任何字节/pong
    auto last_ping = std::chrono::steady_clock::now();    // 服务端心跳节拍
    bool no_rx_warned = false;                            // 静默告警已打（防刷屏）
    WsPeer peer;
    WsCloseReason reason = WsCloseReason::None;
    bool running = true;

    while (running) {
        // 有状态读帧：半帧跨节拍累积，超时只是空闲节拍，不再误断开；
        // 只有 close / 协议错误 / 帧超时 / 底层错误才返回 -1（reason 说明原因）
        uint8_t payload[4096];
        size_t len = 0;
        bool got_any = false;
        int rc = ws_rx_frame(conn, peer, payload, sizeof payload, len, 200, reason, got_any);
        if (got_any) last_rx_any = std::chrono::steady_clock::now();
        if (rc == -1) {
            running = false;
            break;
        }

        if (rc == 0x2 && len >= 2) {  // binary
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
        } else if (rc == 0x1 && len > 0) {  // text（宽松兼容）
            std::string json_str((const char*)payload, len);
            csrc::Json cmd = csrc::Json::parse_or(json_str);
            if (cmd.is_object()) ws_handle_json(ctx, conn, cmd);
        }
        // rc == 0：空闲节拍（ping/pong/close 已在 ws_rx_frame 内处理）

        auto now = std::chrono::steady_clock::now();
        double rx_idle_s = std::chrono::duration<double>(now - last_rx_any).count();
        if (got_any) {
            no_rx_warned = false;  // 收到任何字节(pong/数据)即复位告警
            CAM_DEBUG("[ws] rx tick (data opcode=%d len=%zu)", rc, len);
        }

        // 服务端心跳：4s 一 ping（浏览器自动回 pong，刷活 NAT/中间盒并探活）；
        // 12s 内没收到任何字节（含 pong）→ 判定死链，主动断开避免悬挂
        if (now - last_ping >= std::chrono::milliseconds(4000)) {
            if (!ws_send_frame(conn, 0x9, nullptr, 0)) {
                reason = WsCloseReason::WriteFail;
                running = false;
                break;
            }
            CAM_DEBUG("[ws] ping sent (tx idle %.1fs)", rx_idle_s);
            last_ping = now;
        }
        // 诊断：长时间没收到客户端任何字节时的分级告警（正常浏览器会回 pong）
        if (!no_rx_warned && rx_idle_s >= 6.0) {
            CAM_WARN("[ws] no client bytes for %.0fs (will close at 12s) — 若在摇杆按住状态出现说明 pong 没回来", rx_idle_s);
            no_rx_warned = true;
        }
        if (rx_idle_s >= 12.0) {
            reason = WsCloseReason::PingTimeout;
            running = false;
            break;
        }

        // 每 200ms 推电机状态 0xBB left right (m/s × 1000, int16 LE)
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
                reason = WsCloseReason::WriteFail;
                running = false;
                break;
            }
            last_status = now;
            send_motor_status(false);  // 底盘连接状态变化才推（不刷屏）
        }
    }

    // 断开自动停电机（摇杆松手不跑车）
    run_motor(ctx, 0, 0, 0);
    conn.close();
    CAM_WARN("[ws] client disconnected (reason=%s)", ws_reason_str(reason));
}

}  // namespace capp
