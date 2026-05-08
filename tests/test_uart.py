"""
ESP32-C3 电机控制器 UART 协议测试程序
COM10 | 115200 baud
"""

import serial
import time
import struct
import sys

# ── 配置 ──────────────────────────────────────────────
PORT     = "/dev/cu.usbmodem1201"
BAUD     = 115200
TIMEOUT  = 1.0   # 秒

# ── 命令/响应字 ───────────────────────────────────────
CMD_INIT       = 0x01
CMD_CONFIG     = 0x02
CMD_SET_SPEED  = 0x10
CMD_STOP       = 0x11
CMD_BRAKE      = 0x12
CMD_GET_RPM    = 0x20
CMD_GET_STATUS = 0x21
CMD_RESET      = 0xFF

RSP_ACK        = 0x80
RSP_NACK       = 0x81
RSP_RPM_DATA   = 0x90
RSP_STATUS     = 0x91

ERR_WRONG_STATE   = 0x01
ERR_BAD_CHECKSUM  = 0x02
ERR_INVALID_PARAM = 0x03
ERR_UNKNOWN_CMD   = 0x04

ERR_NAMES = {
    0x01: "WRONG_STATE",
    0x02: "BAD_CHECKSUM",
    0x03: "INVALID_PARAM",
    0x04: "UNKNOWN_CMD",
}

STATE_NAMES = {0: "UNINIT", 1: "IDLE", 2: "READY", 3: "RUNNING", 4: "ERROR"}

PASS = "\033[92m[PASS]\033[0m"
FAIL = "\033[91m[FAIL]\033[0m"
INFO = "\033[94m[INFO]\033[0m"
WARN = "\033[93m[WARN]\033[0m"

# ── 帧构造 / 解析 ────────────────────────────────────

def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    length = len(payload)
    chk = cmd ^ length
    for b in payload:
        chk ^= b
    return bytes([0xAA, 0x55, cmd, length]) + payload + bytes([chk])


def recv_frame(ser: serial.Serial) -> dict | None:
    """阻塞读取一帧，返回 {cmd, payload} 或 None"""
    deadline = time.time() + TIMEOUT
    buf = bytearray()

    # 等帧头
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

    # CMD LEN
    header = ser.read(2)
    if len(header) < 2:
        return None
    cmd, length = header[0], header[1]

    # PAYLOAD
    payload = ser.read(length) if length else b""

    # CHK
    chk_b = ser.read(1)
    if not chk_b:
        return None

    chk = cmd ^ length
    for b in payload:
        chk ^= b
    if chk != chk_b[0]:
        print(f"  {WARN} 校验失败: 期望 {chk:#04x} 收到 {chk_b[0]:#04x}")
        return None

    return {"cmd": cmd, "payload": bytes(payload)}


# ── 测试工具 ─────────────────────────────────────────

class Tester:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, name: str, condition: bool, detail: str = ""):
        if condition:
            print(f"  {PASS} {name}" + (f"  ({detail})" if detail else ""))
            self.passed += 1
        else:
            print(f"  {FAIL} {name}" + (f"  ({detail})" if detail else ""))
            self.failed += 1
        return condition

    def summary(self):
        total = self.passed + self.failed
        print("\n" + "="*50)
        print(f"  测试结果: {self.passed}/{total} 通过")
        if self.failed:
            print(f"  \033[91m{self.failed} 项失败\033[0m")
        else:
            print(f"  \033[92m全部通过！\033[0m")
        print("="*50)


def send_recv(ser, cmd, payload=b"", tester=None, name=""):
    """发送命令并解析第一帧响应"""
    frame = build_frame(cmd, payload)
    ser.reset_input_buffer()
    ser.write(frame)
    resp = recv_frame(ser)
    if tester and name:
        tester.check(f"{name} - 收到响应", resp is not None)
    return resp


def expect_ack(ser, cmd, payload=b"", tester=None, name=""):
    resp = send_recv(ser, cmd, payload)
    if resp is None:
        if tester:
            tester.check(f"{name} - ACK", False, "无响应")
        return False
    ok = resp["cmd"] == RSP_ACK and len(resp["payload"]) >= 1 and resp["payload"][0] == cmd
    if tester:
        detail = f"响应 {resp['cmd']:#04x}" if not ok else ""
        tester.check(f"{name} - ACK", ok, detail)
    return ok


def expect_nack(ser, cmd, payload=b"", expected_err=None, tester=None, name=""):
    resp = send_recv(ser, cmd, payload)
    if resp is None:
        if tester:
            tester.check(f"{name} - NACK", False, "无响应")
        return False
    is_nack = resp["cmd"] == RSP_NACK
    if tester:
        detail = ""
        if is_nack and len(resp["payload"]) >= 2:
            err = resp["payload"][1]
            detail = ERR_NAMES.get(err, f"err={err:#04x}")
            if expected_err and err != expected_err:
                exp_str = ERR_NAMES.get(expected_err, format(expected_err, "#04x"))
                detail += f" (期望 {exp_str})"
                is_nack = False
        tester.check(f"{name} - NACK", is_nack, detail)
    return is_nack


# ── 各项测试 ─────────────────────────────────────────

def test_init(ser, t):
    print(f"\n{INFO} ── 测试 INIT ──")
    expect_ack(ser, CMD_INIT, tester=t, name="INIT")


def test_config(ser, t):
    print(f"\n{INFO} ── 测试 CONFIG ──")
    # 正常配置：PPR=4680 (1:90减速比), FREQ=20000
    payload = struct.pack(">HH", 4680, 20000)
    expect_ack(ser, CMD_CONFIG, payload, tester=t, name="CONFIG(4680, 20000Hz)")

    # CONFIG 之后再次 CONFIG 应该 NACK（状态已是 READY，不是 IDLE）
    # 先重新 INIT → IDLE
    ser.write(build_frame(CMD_INIT)); recv_frame(ser)
    # 非法参数：PPR=0
    payload_bad = struct.pack(">HH", 0, 20000)
    expect_nack(ser, CMD_CONFIG, payload_bad, ERR_INVALID_PARAM, tester=t, name="CONFIG(PPR=0, 非法)")
    # 恢复到 READY
    ser.write(build_frame(CMD_CONFIG, struct.pack(">HH", 4680, 20000))); recv_frame(ser)


def test_get_status(ser, t):
    print(f"\n{INFO} ── 测试 GET_STATUS ──")
    resp = send_recv(ser, CMD_GET_STATUS)
    ok = resp is not None and resp["cmd"] == RSP_STATUS and len(resp["payload"]) == 5
    t.check("GET_STATUS - STATUS 响应", ok)
    if ok:
        state = resp["payload"][0]
        rpm1  = struct.unpack(">h", resp["payload"][1:3])[0]
        rpm2  = struct.unpack(">h", resp["payload"][3:5])[0]
        print(f"       状态: {STATE_NAMES.get(state, state)}  M1={rpm1} RPM  M2={rpm2} RPM")


def test_set_speed(ser, t):
    print(f"\n{INFO} ── 测试 SET_SPEED ──")

    # M1 正转 200
    payload = struct.pack(">Bh", 0, 200)
    expect_ack(ser, CMD_SET_SPEED, payload, tester=t, name="M1 正转 speed=200")
    time.sleep(0.5)

    # M2 正转 200
    payload = struct.pack(">Bh", 1, 200)
    expect_ack(ser, CMD_SET_SPEED, payload, tester=t, name="M2 正转 speed=200")
    time.sleep(0.5)

    # M1 反转 150
    payload = struct.pack(">Bh", 0, -150)
    expect_ack(ser, CMD_SET_SPEED, payload, tester=t, name="M1 反转 speed=-150")
    time.sleep(0.5)

    # 非法 motor_id=5
    payload = struct.pack(">Bh", 5, 100)
    expect_nack(ser, CMD_SET_SPEED, payload, ERR_INVALID_PARAM, tester=t, name="SET_SPEED motor_id=5 非法")

    # 恢复正转方便后续测转速
    ser.write(build_frame(CMD_SET_SPEED, struct.pack(">Bh", 0, 200))); recv_frame(ser)
    ser.write(build_frame(CMD_SET_SPEED, struct.pack(">Bh", 1, 200))); recv_frame(ser)


def test_get_rpm(ser, t):
    print(f"\n{INFO} ── 测试 GET_RPM ──")
    time.sleep(0.3)  # 让电机稳定（1:90减速比需要更长时间）

    # 诊断：读取编码器原始计数 + ISR 触发次数
    print(f"\n{INFO} ── 诊断：编码器 + ISR ──")
    ser.reset_input_buffer()
    ser.write(build_frame(0x30))
    resp = recv_frame(ser)
    if resp and resp["cmd"] == 0x30 and len(resp["payload"]) >= 16:
        c1 = struct.unpack(">l", resp["payload"][0:4])[0]
        c2 = struct.unpack(">l", resp["payload"][4:8])[0]
        i1 = struct.unpack(">L", resp["payload"][8:12])[0]
        i2 = struct.unpack(">L", resp["payload"][12:16])[0]
        print(f"       M1 count={c1}  ISR={i1}  |  M2 count={c2}  ISR={i2}")
        t.check("编码器有计数", c1 != 0 or c2 != 0, f"M1={c1}, M2={c2}")
        t.check("ISR 有触发", i1 != 0 or i2 != 0, f"ISR1={i1}, ISR2={i2}")
    elif resp and resp["cmd"] == 0x30 and len(resp["payload"]) >= 8:
        c1 = struct.unpack(">l", resp["payload"][0:4])[0]
        c2 = struct.unpack(">l", resp["payload"][4:8])[0]
        print(f"       M1 count={c1}  M2 count={c2}  (旧固件，无 ISR 数据)")
        t.check("编码器有计数", c1 != 0 or c2 != 0, f"M1={c1}, M2={c2}")
    else:
        print(f"  {WARN} 无法读取编码器计数")

    # 查 M1
    resp = send_recv(ser, CMD_GET_RPM, bytes([0]))
    ok = resp is not None and resp["cmd"] == RSP_RPM_DATA
    t.check("GET_RPM M1 - 响应格式", ok)
    if ok:
        mid = resp["payload"][0]
        rpm = struct.unpack(">h", resp["payload"][1:3])[0]
        t.check("GET_RPM M1 - 转速非零", rpm != 0, f"M1={rpm} RPM")

    # 查双路（motor_id=2）
    ser.reset_input_buffer()
    ser.write(build_frame(CMD_GET_RPM, bytes([2])))
    r1 = recv_frame(ser)
    r2 = recv_frame(ser)
    ok2 = (r1 is not None and r1["cmd"] == RSP_RPM_DATA and
           r2 is not None and r2["cmd"] == RSP_RPM_DATA)
    t.check("GET_RPM 双路(id=2) - 收到两帧", ok2)
    if ok2:
        rpm1 = struct.unpack(">h", r1["payload"][1:3])[0]
        rpm2 = struct.unpack(">h", r2["payload"][1:3])[0]
        print(f"       M1={rpm1} RPM  M2={rpm2} RPM")


def test_stop_brake(ser, t):
    print(f"\n{INFO} ── 测试 STOP / BRAKE ──")

    expect_ack(ser, CMD_STOP,  bytes([0]), tester=t, name="STOP M1")
    expect_ack(ser, CMD_STOP,  bytes([1]), tester=t, name="STOP M2")
    time.sleep(0.4)

    # 重新给速度再测刹车
    ser.write(build_frame(CMD_SET_SPEED, struct.pack(">Bh", 0, 180))); recv_frame(ser)
    ser.write(build_frame(CMD_SET_SPEED, struct.pack(">Bh", 1, 180))); recv_frame(ser)
    time.sleep(0.3)

    expect_ack(ser, CMD_BRAKE, bytes([0]), tester=t, name="BRAKE M1")
    expect_ack(ser, CMD_BRAKE, bytes([1]), tester=t, name="BRAKE M2")


def test_wrong_state(ser, t):
    print(f"\n{INFO} ── 测试 错误状态拒绝 ──")
    # RESET 回 UNINIT
    ser.write(build_frame(CMD_RESET)); recv_frame(ser)
    time.sleep(0.1)

    # UNINIT 下 SET_SPEED 应 NACK
    payload = struct.pack(">Bh", 0, 100)
    expect_nack(ser, CMD_SET_SPEED, payload, ERR_WRONG_STATE, tester=t,
                name="UNINIT 下 SET_SPEED → NACK(WRONG_STATE)")

    # UNINIT 下 CONFIG 应 NACK
    payload = struct.pack(">HH", 2496, 20000)
    expect_nack(ser, CMD_CONFIG, payload, ERR_WRONG_STATE, tester=t,
                name="UNINIT 下 CONFIG → NACK(WRONG_STATE)")


def test_bad_checksum(ser, t):
    print(f"\n{INFO} ── 测试 错误校验 ──")
    # 构造正常帧后破坏校验位
    frame = bytearray(build_frame(CMD_GET_STATUS))
    frame[-1] ^= 0xFF  # 破坏 CHK
    ser.reset_input_buffer()
    ser.write(frame)
    resp = recv_frame(ser)
    # 协议规定校验错误时设备返回 NACK(BAD_CHECKSUM)
    ok = resp is not None and resp["cmd"] == RSP_NACK
    if ok and len(resp["payload"]) >= 2:
        err = resp["payload"][1]
        t.check("坏校验 → NACK(BAD_CHECKSUM)", err == ERR_BAD_CHECKSUM,
                ERR_NAMES.get(err, f"{err:#04x}"))
    else:
        t.check("坏校验 → NACK", ok)


def test_unknown_cmd(ser, t):
    print(f"\n{INFO} ── 测试 未知命令 ──")
    # 先 INIT，保证状态机能响应
    ser.write(build_frame(CMD_INIT)); recv_frame(ser)
    expect_nack(ser, 0x55, tester=t, name="未知命令 0x55 → NACK")


def test_reset(ser, t):
    print(f"\n{INFO} ── 测试 RESET ──")
    # 先让电机跑起来
    ser.write(build_frame(CMD_INIT)); recv_frame(ser)
    ser.write(build_frame(CMD_CONFIG, struct.pack(">HH", 4680, 20000))); recv_frame(ser)
    ser.write(build_frame(CMD_SET_SPEED, struct.pack(">Bh", 0, 200))); recv_frame(ser)
    time.sleep(0.2)

    expect_ack(ser, CMD_RESET, tester=t, name="RESET")
    time.sleep(0.1)

    # RESET 后状态应为 UNINIT，SET_SPEED 应被拒绝
    payload = struct.pack(">Bh", 0, 100)
    expect_nack(ser, CMD_SET_SPEED, payload, ERR_WRONG_STATE, tester=t,
                name="RESET 后 SET_SPEED → NACK")


# ── 主程序 ───────────────────────────────────────────

def main():
    print("="*50)
    print("  ESP32-C3 电机控制器 UART 协议全面测试")
    print(f"  端口: {PORT}  波特率: {BAUD}")
    print("="*50)

    try:
        ser = serial.Serial(PORT, BAUD, timeout=TIMEOUT)
    except serial.SerialException as e:
        print(f"{FAIL} 打开串口失败: {e}")
        sys.exit(1)

    time.sleep(0.5)  # 等待设备稳定
    ser.reset_input_buffer()

    t = Tester()

    try:
        test_init(ser, t)
        test_config(ser, t)
        test_get_status(ser, t)
        test_set_speed(ser, t)
        test_get_rpm(ser, t)
        test_stop_brake(ser, t)
        test_wrong_state(ser, t)
        test_bad_checksum(ser, t)
        test_unknown_cmd(ser, t)
        test_reset(ser, t)
    finally:
        # 确保测试结束后电机停止
        ser.write(build_frame(CMD_RESET))
        ser.close()

    t.summary()


if __name__ == "__main__":
    main()
