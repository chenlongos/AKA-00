// capp/websocket.hpp — 极简 RFC 6455 WebSocket 服务器（二进制帧）
//
// 仅实现本项目所需子集：
//   - 握手: Sec-WebSocket-Key → Sec-WebSocket-Accept（SHA-1 + base64）
//   - 帧: 二进制(0x2) / 文本(0x1) / ping(0x9) / pong(0xA) / close(0x8)
//   - 客户端帧需 mask；服务端帧不 mask
//   - 不支持分片（FIN=0 → 关闭连接）

#pragma once

#include <cstdint>
#include <string>

#include "capp/http_server.hpp"

namespace capp {

/// 尝试 WebSocket 握手。成功返回 true（conn 已切到 WS 模式）。
bool ws_handshake(const HttpRequest& req, ClientConn& conn);

/// 发送一帧（opcode: 0x1 text / 0x2 binary）。
bool ws_send_frame(ClientConn& conn, uint8_t opcode, const void* data, size_t len);
bool ws_send_binary(ClientConn& conn, const void* data, size_t len);

/// 读取一帧。返回:
///   >0  opcode（payload 写入 out）
///    0  连接关闭/超时
///   -1  协议错误（应断开）
int ws_read_frame(ClientConn& conn, uint8_t* out, size_t cap, size_t& out_len, int timeout_ms);

/// /ws/control 处理器（进入后接管连接直到断开）。
void ws_control_loop(AppContext& ctx, ClientConn& conn);

}  // namespace capp
