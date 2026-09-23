import serial
import time

from src.arm_control.angle_config import load_arm_angles, get_gripper_open, get_gripper_close

# ── 闭环夹爪参数（与 cpp/csrc/src/zp10s.cpp 保持一致）──
# 只影响"夹住之后怎么收手"，不影响张开/手臂/其它角度。板上实测后按需调：
GRIP_POLL_S = 0.05        # PRAD 轮询间隔
GRIP_STABLE_COUNT = 5     # 连续多少次位置不变算"夹住"（5 × 50ms = 250ms）
GRIP_MIN_OFFSET = 20      # 距目标至少差这么多脉宽才算"被挡住了"（≈2.7°）
GRIP_BIAS = 20            # 夹住后保留的位置误差 = 夹持力（大 = 夹得紧、电流大）
GRIP_TIMEOUT_MS = 1600    # 等"夹住"的上限；超时就按普通闭合处理


class ZP10S:
    def __init__(self, port="/dev/ttyS2", baudrate=115200):
        self.ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.1
        )
        self._angles = load_arm_angles("zp10s")

    def update_angles(self, angles):
        """合并新角度到运行时配置。"""
        # 深度合并 grab_position / lift_position
        for group_key in ("grab_position", "lift_position"):
            if group_key in angles and isinstance(angles[group_key], dict):
                if group_key not in self._angles or not isinstance(self._angles[group_key], dict):
                    self._angles[group_key] = {}
                self._angles[group_key] = {**self._angles[group_key], **angles[group_key]}
        for scalar_key in ("gripper_open", "gripper_close"):
            if scalar_key in angles:
                self._angles[scalar_key] = angles[scalar_key]

    def _pos(self, group_key: str, servo_key: str) -> int:
        """读取某个位姿组中某个舵机的角度。"""
        group = self._angles.get(group_key, {})
        if isinstance(group, dict):
            return int(group.get(servo_key, 150))
        return 150

    @property
    def gripper_open_angle(self) -> int:
        return get_gripper_open(self._angles)

    @property
    def gripper_close_angle(self) -> int:
        return get_gripper_close(self._angles)

    def close(self):
        if self.ser.is_open:
            self.ser.close()

    def _send_pulse(self, servo_id, pulse, time_ms):
        pulse = max(500, min(2500, pulse))      # 安全限幅
        time_ms = max(0, min(9999, time_ms))
        cmd = f"#{servo_id:03d}P{pulse:04d}T{time_ms:04d}!"
        self.ser.write(cmd.encode('ascii'))
        self.ser.flush()

    def _send_frame(self, servo_id, angle):
        # 将角度映射到脉宽 500~2500
        self._send_pulse(servo_id, int(500 + (angle / 270.0) * 2000), 1000)

    def _send_cmd(self, servo_id, cmd):
        cmd = f"#{servo_id:03d}{cmd}"
        self.ser.write(cmd.encode('ascii'))
        self.ser.flush()

    def read_position(self, servo_id):
        """读当前位置（脉宽 500~2500）。手册 p.26 第 9 条：#000PRAD! → #000P1500!
        超时/回包不合法（如 "#000P!" 这类无位置回包）返回 None。"""
        self.ser.reset_input_buffer()   # 丢掉上一次残留，否则会把旧回包当成这一次的
        self.ser.write(f"#{servo_id:03d}PRAD!".encode('ascii'))
        self.ser.flush()

        buf = b""
        deadline = time.monotonic() + 0.2
        while time.monotonic() < deadline and b"!" not in buf:
            chunk = self.ser.read(32)
            if chunk:
                buf += chunk

        text = buf.decode('ascii', errors='replace')
        hash_i = text.find('#')
        p_i = text.find('P', hash_i if hash_i >= 0 else 0)
        if p_i < 0:
            return None
        digits = ""
        for ch in text[p_i + 1:]:
            if not ch.isdigit():
                break
            digits += ch
        if not digits:
            return None
        value = int(digits)
        if not 500 <= value <= 2500:    # 挡掉 "#000P!"（ID 检测）这类回包
            return None
        return value

    def grip_until_stall(self, servo_id, angle, bias=GRIP_BIAS, timeout_ms=GRIP_TIMEOUT_MS):
        """闭环闭合夹爪：一边驱动一边用 read_position 盯位置，位置不再变（≈ 已经夹住
        物体）就收手 —— 把目标改到"夹住处再往闭合方向 bias"，不再持续硬顶。

        为什么：夹住东西 = 舵机永远到不了目标 = 持续堵转，而堵转电流 1.8~2A（手册
        p.7/p.14），手册每章的注意事项都写着"合理运行转矩≈1/3 堵转扭矩"。硬顶下去
        要么触发防堵转释力（= 松劲），要么烧。收手之后靠位置误差维持夹持力，
        电流随 bias 走 —— bias 就是"夹多紧"那个旋钮。

        返回夹住时的脉宽；没夹到东西（正常走到目标）返回 None。"""
        target = int(500 + (angle / 270.0) * 2000)
        self._send_pulse(servo_id, target, 1000)

        deadline = time.monotonic() + timeout_ms / 1000.0
        last = None
        stable = 0
        while time.monotonic() < deadline:
            time.sleep(GRIP_POLL_S)

            pos = self.read_position(servo_id)
            if pos is None:     # 这一拍没读到，不算"不动"
                continue

            stable = stable + 1 if (last is not None and abs(pos - last) <= 2) else 0
            last = pos

            # 一直没动 + 又明显没走到目标 —— 两者同时成立才算被物体挡住了
            if stable < GRIP_STABLE_COUNT or abs(pos - target) < GRIP_MIN_OFFSET:
                continue

            direction = -1 if target < pos else 1   # 往"更闭合"的方向
            hold = pos + direction * bias           # 收手：停在夹住处，只留 bias 的预紧
            self._send_pulse(servo_id, hold, 200)
            print(f"[zp10s] 夹住：停在 {pos}（目标 {target}），改持 {hold}"
                  f"（bias {bias}，误差即夹持力）")
            return pos

        return None     # 没夹到东西（空合），或这颗固件不回 PRAD

    def _send_raw_cmd(self, cmd):
        self.ser.write(cmd.encode('ascii'))
        self.ser.flush()

    # 手册 p.25 明写 "#" 和 "!" 是固定英文格式 —— 少了 "!" 就是残帧，控制板不会执行。
    def release_torque(self):
        self._send_cmd(255, "PULK!")

    def restoring_torque(self):
        self._send_cmd(255, "PULR!")

    def set_angle(self, servo_id, angle):
        if not 0 <= angle <= 270:
            raise ValueError("angle must be 0~270")
        self._send_frame(servo_id, angle)


def grab(servo):
    """抓取动作：张开夹爪 → 夹取位姿 → 闭合夹爪 → 抬起位姿"""
    # 1. 张开夹爪
    servo.set_angle(2, servo.gripper_open_angle)
    time.sleep(0.5)

    # 2. 夹取位姿（手臂舵机到位）
    servo.set_angle(0, servo._pos("grab_position", "servo0"))
    servo.set_angle(1, servo._pos("grab_position", "servo1"))
    time.sleep(1)

    # 3. 闭合夹爪 —— 闭环：夹住就收手，不再持续硬顶（见 grip_until_stall 注释）
    #    这一步本身就阻塞到"夹住"或超时，所以后面不用再干等 2 秒。
    if servo.grip_until_stall(2, servo.gripper_close_angle) is None:
        print("[zp10s] 夹爪闭合：没读到夹住（空合，或固件不回 PRAD）")
    time.sleep(0.2)

    # 4. 抬起位姿（手臂舵机抬起）
    servo.set_angle(0, servo._pos("lift_position", "servo0"))
    servo.set_angle(1, servo._pos("lift_position", "servo1"))


def release(servo):
    """张开夹爪。"""
    servo.set_angle(2, servo.gripper_open_angle)
