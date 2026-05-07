"""
ESP32-C3 编码器独立测试
手动拨动轮子，观察编码器计数和 RPM 变化
"""

import serial
import struct
import time
import sys

PORT = "/dev/cu.usbmodem1201"
BAUD = 115200
PPR  = 4680

CMD_INIT      = 0x01
CMD_CONFIG    = 0x02
CMD_GET_RPM   = 0x20
CMD_RESET     = 0xFF
RSP_RPM_DATA  = 0x90


def build_frame(cmd, payload=b""):
    length = len(payload)
    chk = cmd ^ length
    for b in payload:
        chk ^= b
    return bytes([0xAA, 0x55, cmd, length]) + payload + bytes([chk])


def recv_frame(ser, timeout=0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b[0] == 0xAA:
            b2 = ser.read(1)
            if b2 and b2[0] == 0x55:
                break
    else:
        return None
    header = ser.read(2)
    if len(header) < 2:
        return None
    cmd, length = header[0], header[1]
    payload = ser.read(length) if length else b""
    chk_b = ser.read(1)
    if not chk_b:
        return None
    chk = cmd ^ length
    for b in payload:
        chk ^= b
    if chk != chk_b[0]:
        return None
    return {"cmd": cmd, "payload": bytes(payload)}


def read_debug(ser):
    ser.reset_input_buffer()
    ser.write(build_frame(0x30))
    resp = recv_frame(ser, 0.5)
    if resp and resp["cmd"] == 0x30:
        pl = resp["payload"]
        c1 = struct.unpack(">l", pl[0:4])[0]
        c2 = struct.unpack(">l", pl[4:8])[0]
        i1 = struct.unpack(">L", pl[8:12])[0] if len(pl) >= 16 else -1
        i2 = struct.unpack(">L", pl[12:16])[0] if len(pl) >= 16 else -1
        return c1, c2, i1, i2
    return None, None, None, None


def read_rpm(ser):
    r = None, None
    ser.reset_input_buffer()
    ser.write(build_frame(CMD_GET_RPM, bytes([0])))
    r1 = recv_frame(ser, 0.3)
    ser.write(build_frame(CMD_GET_RPM, bytes([1])))
    r2 = recv_frame(ser, 0.3)
    rpm1 = struct.unpack(">h", r1["payload"][1:3])[0] if r1 and r1["cmd"] == RSP_RPM_DATA else None
    rpm2 = struct.unpack(">h", r2["payload"][1:3])[0] if r2 and r2["cmd"] == RSP_RPM_DATA else None
    return rpm1, rpm2


def main():
    print("=" * 56)
    print("  ESP32-C3 编码器独立测试")
    print("  手动拨动轮子，观察计数 / RPM 变化")
    print("=" * 56)

    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.1)
    except serial.SerialException as e:
        print(f"打开串口失败: {e}")
        sys.exit(1)

    time.sleep(0.5)
    ser.reset_input_buffer()

    # 只做 INIT + CONFIG，不启动电机
    ser.write(build_frame(CMD_INIT))
    recv_frame(ser)
    ser.write(build_frame(CMD_CONFIG, struct.pack(">HH", PPR, 20000)))
    recv_frame(ser)
    print("\n已初始化，电机未启动。现在拨动轮子！\n")
    time.sleep(0.3)

    try:
        while True:
            c1, c2, i1, i2 = read_debug(ser)
            rpm1, rpm2 = read_rpm(ser)

            line = f"  M1  count={c1 if c1 is not None else '?':>8}  ISR={i1 if i1 is not None else '?':>8}  RPM={rpm1 if rpm1 is not None else '?':>6}   |   "
            line += f"M2  count={c2 if c2 is not None else '?':>8}  ISR={i2 if i2 is not None else '?':>8}  RPM={rpm2 if rpm2 is not None else '?':>6}"
            print(f"\r{line}", end="", flush=True)
            time.sleep(0.001)

    except KeyboardInterrupt:
        ser.write(build_frame(CMD_RESET))
        ser.close()
        print("\n\n已断开连接")


if __name__ == "__main__":
    main()
