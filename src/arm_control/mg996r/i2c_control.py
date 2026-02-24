import time

from periphery import I2C

I2C_BUS = "/dev/i2c-1"
ADDR = 0x40

class MG996R:
    MODE1 = 0x00
    MODE2 = 0x01
    PRESCALE = 0xFE
    LED0_ON_L = 0x06

    def __init__(self, bus="/dev/i2c-1", addr=0x40):
        self.addr = addr
        self.i2c = I2C(bus)

    def _write(self, reg, data):
        if isinstance(data, int):
            msg = I2C.Message([reg, data & 0xFF])
        else:
            msg = I2C.Message([reg] + list(data))
        self.i2c.transfer(self.addr, [msg])


    def init_50hz(self):
        # sleep
        self._write(self.MODE1, 0x10)
        time.sleep(0.005)

        # prescale = 121 -> 50Hz
        self._write(self.PRESCALE, 0x79)

        # wake + auto increment
        self._write(self.MODE1, 0x20)
        time.sleep(0.001)

        # push-pull output (推荐)
        self._write(self.MODE2, 0x04)

    def set_pwm(self, ch, on, off):
        reg = self.LED0_ON_L + 4 * ch
        self._write(reg, [
            on & 0xFF,
            on >> 8,
            off & 0xFF,
            off >> 8,
        ])

    def set_servo_us(self, ch, us):
        # 50Hz: 1 tick ≈ 4.88us
        tick = int(us / 4.88)
        self.set_pwm(ch, 0, tick)

def main():
    pwm = MG996R("/dev/i2c-1", 0x40)
    pwm.init_50hz()

    pwm.set_servo_us(0, 1500)  # 中位
    time.sleep(1)
    pwm.set_servo_us(1, 1500)  # 中位
    time.sleep(1)
    pwm.set_servo_us(0, 500)  # 左
    time.sleep(1)
    pwm.set_servo_us(1, 500)  # 左
    time.sleep(1)
    pwm.set_servo_us(0, 2500)  # 右
    time.sleep(1)
    pwm.set_servo_us(1, 2500)  # 右

if __name__ == '__main__':
    main()