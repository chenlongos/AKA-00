#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# tools/ws_joystick_test.py — 模拟前端 WebSocket 摇杆协议，直接验证 WS→底盘链路
#
# 板上用法（需 python3）:
#   python3 ws_joystick_test.py            # 前进 y=40，2 秒
#   python3 ws_joystick_test.py 50 3000    # y=50，3 秒
#
# 行为: 连 ws://127.0.0.1/ws/control → 发 0xAA [x,y] 摇杆帧 → 期间打印
#       服务端推的 0xBB 状态 → 断开（服务端自动停电机）
#
# 判断:
#   1. 打印 welcome 且 0xBB 状态值变化 + 小车动 → WS→底盘链路通，问题在前端页面
#   2. 连不上 / 握手失败 → capp 的 WS 有问题
#   3. 小车不动但 0xBB 正常 → 控制命令转换/执行问题

import base64, hashlib, os, socket, struct, sys, time

def main():
    y = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0

    s = socket.create_connection(("127.0.0.1", 80), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws/control HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = s.recv(4096).decode(errors="replace")
    if "101" not in resp.split("\r\n")[0]:
        print("握手失败:", resp[:200]); return 1
    print("握手 OK")

    def read_frame():
        hdr = s.recv(2)
        if len(hdr) < 2: return None, None
        op = hdr[0] & 0x0F; ln = hdr[1] & 0x7F
        if ln == 126: ln = struct.unpack(">H", s.recv(2))[0]
        elif ln == 127: ln = struct.unpack(">Q", s.recv(8))[0]
        payload = b""
        while len(payload) < ln: payload += s.recv(ln - len(payload))
        return op, payload

    def send_frame(opcode, payload):
        mask = os.urandom(4)
        hdr = bytes([0x80 | opcode])
        if len(payload) < 126: hdr += bytes([0x80 | len(payload)])
        else: hdr += bytes([0x80 | 126]) + struct.pack(">H", len(payload))
        s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    # 读 welcome（0xDD ip）
    op, p = read_frame()
    if p and p[0] == 0xDD:
        print("welcome:", p[1:].decode(errors="replace")[:80])

    # 发摇杆: [0xAA, x=0, y]
    x = 0
    print(f"发送 joystick x={x} y={y}（前进，保持 {dur}s）")
    send_frame(0x2, bytes([0xAA, x & 0xFF, y & 0xFF]))

    # 期间读 0xBB 状态
    s.settimeout(0.5)
    t0 = time.time()
    while time.time() - t0 < dur:
        try:
            op, p = read_frame()
            if p and p[0] == 0xBB and len(p) >= 5:
                l, r = struct.unpack("<hh", p[1:5])
                print(f"0xBB 左={l/1000:.2f}m/s 右={r/1000:.2f}m/s")
        except socket.timeout:
            pass

    s.close()
    print("断开（capp 会自动停电机）")
    return 0

if __name__ == "__main__":
    sys.exit(main())
