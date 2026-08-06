import serial
import time

from src.arm_control.angle_config import load_arm_angles, get_gripper_open, get_gripper_close


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

    def _send_frame(self, servo_id, angle):
        # 将角度映射到脉宽 500~2500
        pulse = int(500 + (angle / 270.0) * 2000)
        pulse = max(500, min(2500, pulse))  # 安全限幅
        cmd = f"#{servo_id:03d}P{pulse:04d}T{1000}!"
        self.ser.write(cmd.encode('ascii'))
        self.ser.flush()

    def _send_cmd(self, servo_id, cmd):
        cmd = f"#{servo_id:03d}{cmd}"
        self.ser.write(cmd.encode('ascii'))
        self.ser.flush()

    def _send_raw_cmd(self, cmd):
        self.ser.write(cmd.encode('ascii'))
        self.ser.flush()

    def release_torque(self):
        self._send_cmd(255, "PULK")

    def restoring_torque(self):
        self._send_cmd(255, "PULR")

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

    # 3. 闭合夹爪
    servo.set_angle(2, servo.gripper_close_angle)
    time.sleep(2)

    # 4. 抬起位姿（手臂舵机抬起）
    servo.set_angle(0, servo._pos("lift_position", "servo0"))
    servo.set_angle(1, servo._pos("lift_position", "servo1"))


def release(servo):
    """张开夹爪。"""
    servo.set_angle(2, servo.gripper_open_angle)
