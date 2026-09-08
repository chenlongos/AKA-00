// capp/websocket.hpp — 极简 RFC 6455 WebSocket 服务器（二进制帧）
//
// 仅实现本项目所需子集：
//   - 握手: Sec-WebSocket-Key → Sec-WebSocket-Accept（SHA-1 + base64）
//   - 帧: 二进制(0x2) / 文本(0x1) / ping(0x9) / pong(0xA) / close(0x8)
//   - 客户端帧需 mask；服务端帧不 mask
//   - 不支持分片（FIN=0 → 关闭连接）

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "capp/http_server.hpp"

namespace capp {

/// 尝试 WebSocket 握手。成功返回 true（conn 已切到 WS 模式）。
bool ws_handshake(const HttpRequest& req, ClientConn& conn);

/// 发送一帧（opcode: 0x1 text / 0x2 binary / 0x9 ping）。
bool ws_send_frame(ClientConn& conn, uint8_t opcode, const void* data, size_t len);
bool ws_send_binary(ClientConn& conn, const void* data, size_t len);

/// 断开原因（诊断"WS 易断"用：每次断开打日志说明原因）
enum class WsCloseReason {
    None = 0,
    ClientClose,   // 客户端发 close 帧 / 正常关闭
    ReadError,     // 底层读错误（TCP RST / TLS 错误）
    Oversize,      // 帧超过缓冲区上限
    Fragmented,    // FIN=0 分片（不支持）
    FrameTimeout,  // 一帧超过 kFrameTimeoutMs 没收完整
    PingTimeout,   // 长时间没有任何字节（含 pong）→ 判定死链
    WriteFail,     // 服务端下发失败（对端关闭）
};

/// 有状态 WS 接收器：跨节拍缓冲字节，绝不因"半帧恰好超时"断开；
/// 只有整帧超过帧级超时或协议错误才断开。
struct WsPeer {
    std::vector<uint8_t> buf;           // 未解析完的字节（可含多个帧）
    bool frame_started = false;         // 当前帧已收到首字节
    std::chrono::steady_clock::time_point frame_t0;
};

/// 推进 WS 接收。每次最多等 timeout_ms 收数据并尝试解析一个完整帧。
/// 返回:
///   >0  opcode（0x1/0x2，payload 已解 mask 写入 out/out_len）
///    0  无完整数据帧（空闲/已内部处理 ping/pong，连接仍可用）
///   -1  应断开，reason 说明原因
int ws_rx_frame(ClientConn& conn, WsPeer& peer, uint8_t* out, size_t cap,
                size_t& out_len, int timeout_ms, WsCloseReason& reason,
                bool& got_any_byte);

/// /ws/control 处理器（进入后接管连接直到断开）。
void ws_control_loop(AppContext& ctx, ClientConn& conn);

}  // namespace capp
