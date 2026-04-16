import serial
import time

from src.arm_control.angle_config import load_arm_angles


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
        self._angles = {**self._angles, **angles}

    def _angle(self, key, default):
        return self._angles.get(key, default)

    @property
    def id2_angle_open(self):
        return self._angle("servo2_prepare", 150)

    @property
    def id2_angle_close(self):
        return self._angle("servo2_grab", 90)

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
    servo.set_angle(2,servo.id2_angle_open)
    time.sleep(0.5)
    servo.set_angle(0, servo._angle("servo0_prepare", 245))
    servo.set_angle(1, servo._angle("servo1_prepare", 180))
    servo.set_angle(2, servo._angle("servo2_approach", 150))
    time.sleep(1)
    servo.set_angle(2,servo.id2_angle_close)
    time.sleep(1)
    servo.set_angle(0, servo._angle("servo0_lift", 200))
    servo.set_angle(1, servo._angle("servo1_lift", 180))
    servo.set_angle(2, servo._angle("servo2_lift", 90))
def release_pos(servo):
    servo.set_angle(0,140)
    servo.set_angle(1,220)
    servo.set_angle(2,servo.id2_angle_close)
def grab_test(servo):
    grab(servo)
    time.sleep(2)
    release_pos(servo)
    time.sleep(2)
    servo.set_angle(0,70)
    servo.set_angle(1,230)
    servo.set_angle(2,servo.id2_angle_close)
    time.sleep(2)
    servo.set_angle(2,servo.id2_angle_open)

def release(servo):
    servo.set_angle(2,servo.id2_angle_open)

def main():
    zp10s = ZP10S()
    print("ZP10S 转到135")
    zp10s.set_angle(1, 135)
    time.sleep(2)
    print("ZP10S 转到20")
    zp10s.set_angle(1, 20)
    time.sleep(2)
    print("ZP10S 转到90")
    zp10s.set_angle(1, 90)
    time.sleep(2)
    print("ZP10S 转到250")
    zp10s.set_angle(1, 250)


if __name__ == '__main__':
    main()
