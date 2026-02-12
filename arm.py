import serial
import time


class STS3215:
    def __init__(self, port="/dev/ttyS2", baudrate=115200):  # 已修改为115200
        self.ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            timeout=0.1
        )
        self.ser.flushInput()
        self.ser.flushOutput()

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
        
        # 等待响应
        start_time = time.time()
        while time.time() - start_time < 1.0:  # 最多等待1秒
            if self.ser.in_waiting >= 6 + length:  # 响应包的基本长度+数据长度
                response = self.ser.read(6 + length)
                
                # 验证响应包头
                if (len(response) >= 6 and 
                    response[0] == 0xFF and 
                    response[1] == 0xFF and 
                    response[2] == servo_id and 
                    response[4] == 0x00):  # 错误码为0表示成功
                    
                    # 提取数据部分
                    data_start = 5
                    data_end = data_start + length
                    if data_end <= len(response):
                        return response[data_start:data_end]
                else:
                    # 如果不是有效响应，继续读取
                    continue
        return None

    def move_to_position(self, servo_id, pos):
        # 限制范围在 0-4095
        pos = max(0, min(4095, int(pos)))
        data = pos.to_bytes(2, 'little')
        # 0x2A (42) 是目标位置寄存器
        self.write_reg(servo_id, 0x2A, data)

    def get_position(self, servo_id):
        """
        获取舵机当前位置
        :param servo_id: 舵机ID
        :return: 当前位置值（0-4095），如果读取失败则返回None
        """
        # 读取当前位置寄存器（0x38）
        position_data = self.read_data(servo_id, 0x38, 2)
        
        if position_data is not None and len(position_data) == 2:
            # 小端序转换
            position = int.from_bytes(position_data, byteorder='little')
            return position
        else:
            print(f"无法读取舵机 {servo_id} 的位置")
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
    for i in range(1,4):
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
    servo.move_to_position(3, 4000)
    time.sleep(0.7)
    servo.move_to_position(1, 1800)
    servo.move_to_position(2, 2400)
    servo.move_to_position(3, 4000)
    time.sleep(1)
    servo.move_to_position(3, 3300)
    time.sleep(1)
    servo.move_to_position(1, 2600)
    servo.move_to_position(2, 2500)
    servo.move_to_position(3, 3300)

def grab1(servo):
    servo.move_to_position(1, 1850)
    servo.move_to_position(2, 2650)
    servo.move_to_position(3, 4000)
    time.sleep(1)
    servo.move_to_position(3, 3000)
    time.sleep(1)
    servo.move_to_position(1, 2450)
    servo.move_to_position(2, 2100)
    servo.move_to_position(3, 3000)

def grab_test(servo):
    servo.move_to_position(1, 1850)
    servo.move_to_position(2, 2650)
    servo.move_to_position(3, 4000)
    time.sleep(1)
    servo.move_to_position(3, 3000)
    time.sleep(1)
    servo.move_to_position(1, 2450)
    servo.move_to_position(2, 2100)
    servo.move_to_position(3, 3000)
    time.sleep(2)
    servo.move_to_position(1, 1850)
    servo.move_to_position(2, 2650)
    servo.move_to_position(3, 3000)
    time.sleep(1)
    servo.move_to_position(3, 4000)

def grab_pos(servo):
    servo.move_to_position(3, 4000)
    time.sleep(0.5)
    servo.move_to_position(1, 2300)
    servo.move_to_position(2, 2100)
    servo.move_to_position(3, 4000)

def release_pos(servo):
    servo.move_to_position(1, 2700)
    servo.move_to_position(2, 2600)
    servo.move_to_position(3, 3000)

def release(servo):
    servo.move_to_position(3, 3700)

def main():
    # 实例化，注意波特率必须与系统设置及电机设置一致
    servo = STS3215("/dev/ttyACM0", baudrate=115200)

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
