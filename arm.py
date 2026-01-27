import serial
import time


class STS3215:
    def __init__(self, port="/dev/ttyS2", baudrate=115200):  # 已修改为115200
        self.ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            timeout=0.1
        )

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
        self.ser.write(pkt)

    def write_reg(self, servo_id, addr, data: bytes):
        params = bytes([addr]) + data
        self.send_cmd(servo_id, 0x03, params)  # INST_WRITE

    def move_to_position(self, servo_id, pos):
        # 限制范围在 0-4095
        pos = max(0, min(4095, int(pos)))
        data = pos.to_bytes(2, 'little')
        # 0x2A (42) 是目标位置寄存器
        self.write_reg(servo_id, 0x2A, data)

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
    for i in range(1,4):
        servo.set_operating_mode(i, 0)
        servo.set_speed(i, 1500)
        servo.set_p_coefficient(i, 16)
        servo.set_i_coefficient(i, 0)
        servo.set_d_coefficient(i, 32)

        if i == 3:
            servo.set_max_torque_limit(i, 500)
            servo.set_protection_current(i, 250)
            servo.set_overload_torque(i, 25)

def grab(servo):
    servo.move_to_position(1, 1800)
    servo.move_to_position(2, 2400)
    servo.move_to_position(3, 4000)
    time.sleep(1)
    servo.move_to_position(3, 3300)
    time.sleep(1)
    servo.move_to_position(1, 2600)
    servo.move_to_position(2, 2500)
    servo.move_to_position(3, 3300)

def release(servo):
    servo.move_to_position(3, 3700)


def main():
    # 实例化，注意波特率必须与系统设置及电机设置一致
    servo = STS3215("/dev/ttyS2", baudrate=115200)

    arm_init(servo)

    # grab(servo)

    # release(servo)

    # 初始位置
    # servo.move_to_position(1, 2600)
    # servo.move_to_position(2, 2500)
    # servo.move_to_position(3, 3000)

    # 夹取位置
    # servo.move_to_position(1, 1800)
    # servo.move_to_position(2, 2500)
    # servo.move_to_position(3, 4000)

if __name__ == '__main__':
    main()
