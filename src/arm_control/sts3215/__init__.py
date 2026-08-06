import serial
import time

from src.arm_control.angle_config import load_arm_angles, get_gripper_open, get_gripper_close


class STS3215:
    def __init__(self, port="/dev/ttyS2", baudrate=115200):
        self.ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            timeout=0.1
        )
        self.ser.flushInput()
        self.ser.flushOutput()
        self._angles = load_arm_angles("sts3215")

    def update_angles(self, angles):
        """合并新角度到运行时配置。"""
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
            return int(group.get(servo_key, 4000))
        return 4000

    @property
    def gripper_open_angle(self) -> int:
        return get_gripper_open(self._angles, "sts3215")

    @property
    def gripper_close_angle(self) -> int:
        return get_gripper_close(self._angles, "sts3215")

    def checksum(self, data: bytes) -> int:
        return (~sum(data)) & 0xFF

    def send_cmd(self, servo_id, instruction, params: bytes):
        length = len(params) + 2
        pkt = bytearray()
        pkt += b'\xFF\xFF'
        pkt.append(servo_id)
        pkt.append(length)
        pkt.append(instruction)
        pkt += params
        pkt.append(self.checksum(pkt[2:]))
        self.ser.flushInput()
        self.ser.write(pkt)
        time.sleep(0.005)

    def write_reg(self, servo_id, addr, data: bytes):
        params = bytes([addr]) + data
        self.send_cmd(servo_id, 0x03, params)  # INST_WRITE

    def read_data(self, servo_id, addr, length):
        """读取指定地址的数据"""
        params = bytes([addr, length])
        self.send_cmd(servo_id, 0x02, params)  # INST_READ

        start_time = time.time()
        while time.time() - start_time < 1.0:
            if self.ser.in_waiting >= 6 + length:
                response = self.ser.read(6 + length)

                if (len(response) >= 6 and
                    response[0] == 0xFF and
                    response[1] == 0xFF and
                    response[2] == servo_id and
                    response[4] == 0x00):

                    data_start = 5
                    data_end = data_start + length
                    if data_end <= len(response):
                        return response[data_start:data_end]
                else:
                    continue
        return None

    def move_to_position(self, servo_id, pos):
        pos = max(0, min(4095, int(pos)))
        data = pos.to_bytes(2, 'little')
        self.write_reg(servo_id, 0x2A, data)

    def get_position(self, servo_id):
        position_data = self.read_data(servo_id, 0x38, 2)
        if position_data is not None and len(position_data) == 2:
            position = int.from_bytes(position_data, byteorder='little')
            return position
        else:
            return None

    def move_angle(self, servo_id, angle):
        pos = (angle / 360.0) * 4095
        self.move_to_position(servo_id, pos)

    def set_speed(self, servo_id, speed):
        data = int(speed).to_bytes(2, 'little')
        self.write_reg(servo_id, 0x2E, data)

    def set_max_torque_limit(self, servo_id, torque):
        data = int(torque).to_bytes(2, 'little')
        self.write_reg(servo_id, 0x10, data)

    def set_protection_current(self, servo_id, torque):
        data = int(torque).to_bytes(2, 'little')
        self.write_reg(servo_id, 0x40, data)

    def set_overload_torque(self, servo_id, torque):
        data = int(torque).to_bytes(1, 'little')
        self.write_reg(servo_id, 0x24, data)

    def set_operating_mode(self, servo_id, mode):
        data = int(mode).to_bytes(1, 'little')
        self.write_reg(servo_id, 0x21, data)

    def set_p_coefficient(self, servo_id, mode):
        data = int(mode).to_bytes(1, 'little')
        self.write_reg(servo_id, 0x15, data)

    def set_i_coefficient(self, servo_id, mode):
        data = int(mode).to_bytes(1, 'little')
        self.write_reg(servo_id, 0x17, data)

    def set_d_coefficient(self, servo_id, mode):
        data = int(mode).to_bytes(1, 'little')
        self.write_reg(servo_id, 0x16, data)


def arm_init(servo):
    for i in range(1, 4):
        servo.set_operating_mode(i, 0)
        servo.set_speed(i, 1500)
        servo.set_p_coefficient(i, 16)
        servo.set_i_coefficient(i, 0)
        servo.set_d_coefficient(i, 32)

        if i == 3 or i == 1:
            servo.set_max_torque_limit(i, 500)
            servo.set_protection_current(i, 250)
            servo.set_overload_torque(i, 25)


def grab(servo):
    """抓取动作：张开夹爪 → 抬起位姿(安全) → 夹取位姿 → 闭合夹爪 → 抬起位姿"""
    # 1. 张开夹爪 + 抬起位姿作为中间安全位姿
    servo.move_to_position(3, servo.gripper_open_angle)
    servo.move_to_position(2, servo._pos("lift_position", "servo2"))
    servo.move_to_position(1, servo._pos("lift_position", "servo1"))
    time.sleep(0.4)

    # 2. 夹取位姿（手臂舵机到位，夹爪保持张开）
    servo.move_to_position(1, servo._pos("grab_position", "servo1"))
    servo.move_to_position(2, servo._pos("grab_position", "servo2"))
    time.sleep(1)

    # 3. 闭合夹爪
    servo.move_to_position(3, servo.gripper_close_angle)
    time.sleep(1)

    # 4. 抬起位姿（手臂舵机抬起，夹爪保持闭合）
    servo.move_to_position(1, servo._pos("lift_position", "servo1"))
    servo.move_to_position(2, servo._pos("lift_position", "servo2"))


def grab_pos(servo):
    servo.move_to_position(2, servo._pos("lift_position", "servo2"))
    servo.move_to_position(1, servo._pos("lift_position", "servo1"))
    time.sleep(0.4)
    servo.move_to_position(2, servo._pos("lift_position", "servo2"))
    servo.move_to_position(3, servo._pos("lift_position", "servo3"))


def release_pos(servo):
    servo.move_to_position(1, servo._pos("lift_position", "servo1"))
    servo.move_to_position(2, servo._pos("lift_position", "servo2"))
    time.sleep(0.5)
    servo.move_to_position(2, servo._pos("lift_position", "servo2"))
    servo.move_to_position(3, servo.gripper_close_angle)


def grab_prepare(servo):
    servo.move_to_position(2, servo._pos("lift_position", "servo2"))
    time.sleep(0.4)


def release(servo):
    """张开夹爪。"""
    servo.move_to_position(1, servo._pos("lift_position", "servo1"))
    time.sleep(0.5)
    servo.move_to_position(3, servo.gripper_open_angle)
