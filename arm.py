import serial
import time


class SG2002_STS3215:
    def __init__(self, port='/dev/ttyS2', baudrate=1000000):
        """
        初始化 SG2002 的 UART2
        """
        try:
            self.ser = serial.Serial(
                port=port,
                baudrate=baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=0.1
            )
        except Exception as e:
            print(f"串口打开失败: {e}")

    def _calculate_checksum(self, packet_data):
        """
        packet_data: 包含 ID, Length, Instruction, Params 的列表
        """
        checksum = ~(sum(packet_data) & 0xFF) & 0xFF
        return checksum

    def send_command(self, servo_id, instruction, parameters):
        """
        基础指令发送函数
        """
        # Length = 参数长度 + 2 (Instruction + Checksum)
        length = len(parameters) + 2

        # 准备校验和的数据部分
        check_data = [servo_id, length, instruction] + parameters
        checksum = self._calculate_checksum(check_data)

        # 组装完整报文: 0xFF, 0xFF, ID, Length, Instruction, Params..., Checksum
        packet = bytearray([0xFF, 0xFF] + check_data + [checksum])

        self.ser.write(packet)
        # SG2002 性能较强，如果连续发送过快，可以微调 flush
        self.ser.flush()

    def write_reg(self, servo_id, reg_addr, data_bytes):
        """
        对应 C++ 的 sts3215_cmd_write
        """
        # 指令: 0x03 (WRITE_DATA)
        # 参数: 寄存器地址 + 数据
        params = [reg_addr] + data_bytes
        self.send_command(servo_id, 0x03, params)

    def move_by_angle(self, servo_id, angle):
        """
        对应 C++ 的 sts3215_move_by_angle
        angle: 0-360 度 (或根据舵机量程定义)
        """
        # 转换比例，STS3215 通常是 4096 步对应 360度
        ele = 4096 / 360
        pos = int(angle * ele)

        # 限制范围 (0-4095)
        pos = max(0, min(4095, pos))

        # 拆分为两个字节 (小端模式)
        param = [pos & 0xFF, (pos >> 8) & 0xFF]
        # 0x2A 是目标位置寄存器起始地址
        self.write_reg(servo_id, 0x2A, param)

    def set_speed(self, servo_id, speed):
        """
        对应 C++ 的 sts3215_set_speed
        """
        # 限制速度范围 (根据协议手册调整)
        if speed > 254:
            speed = 254

        param = [speed & 0xFF, (speed >> 8) & 0xFF]
        # 0x2E 是目标速度寄存器起始地址
        self.write_reg(servo_id, 0x2E, param)


# --- SG2002 运行示例 ---
if __name__ == "__main__":
    # 确保 SG2002 的 pinmux 已配置 UART2 模式
    # 通常在 boot 脚本或设备树中配置
    bus = SG2002_STS3215(port='/dev/ttyS2', baudrate=1000000)

    # 示例操作
    servo_id = 1

    print("设置速度...")
    bus.set_speed(servo_id, 100)

    time.sleep(0.1)

    print("移动到 90 度...")
    bus.move_by_angle(servo_id, 90.0)