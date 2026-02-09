import serial
import time


class MG996R:
    FRAME_HEAD = 0xFA
    FRAME_TAIL = 0xFE
    CMD_RESET = 0xFB

    def __init__(self, port="/dev/ttyS2", baudrate=9600):
        self.ser = serial.Serial(
            port=port,  # 改成你实际的 UART
            baudrate=baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.1
        )

    def close(self):
        if self.ser.is_open:
            self.ser.close()

    def _send_frame(self, b1, b2, b3):
        frame = bytes([self.FRAME_HEAD, b1 & 0xFF, b2 & 0xFF, b3 & 0xFF, self.FRAME_TAIL])
        self.ser.write(frame)
        self.ser.flush()

    def set_angle(self, servo_id, angle, adder=0):
        if not 0 <= angle <= 180:
            raise ValueError("angle must be 0~180")
        self._send_frame(adder, servo_id, angle)

    def reset(self, adder):
        self._send_frame(adder, self.CMD_RESET, self.CMD_RESET)

def main():
    mg996r = MG996R()
    mg996r.reset(0)
    mg996r.set_angle(0, 90)
    time.sleep(1)
    mg996r.set_angle(0, 0)
    time.sleep(1)
    mg996r.set_angle(1, 90)
    time.sleep(1)
    mg996r.set_angle(1, 0)


if __name__ == '__main__':
    main()
