import time
from periphery import PWM

# 配置常量
PERIOD_NS = 10000

class Motor:
    def __init__(self, chip, ch1, ch2):
        # 硬件初始化
        self.pwm1 = PWM(chip, ch1)
        self.pwm2 = PWM(chip, ch2)
        for p in (self.pwm1, self.pwm2):
            p.period_ns = PERIOD_NS
            p.duty_cycle_ns = 5000
            p.enable()

    def set_speed(self, speed):
        speed = max(-255, min(255, speed))
        duty = int(abs(speed) * PERIOD_NS // 255)
        if speed == 0:
            self.pwm1.duty_cycle_ns = 0
            self.pwm2.duty_cycle_ns = 0
        elif speed > 0:
            self.pwm1.duty_cycle_ns = duty
            self.pwm2.duty_cycle_ns = 0
        else:
            self.pwm1.duty_cycle_ns = 0
            self.pwm2.duty_cycle_ns = duty

    def brake(self, val=255):
        duty = int(val * PERIOD_NS // 255)
        self.pwm1.duty_cycle_ns = duty
        self.pwm2.duty_cycle_ns = duty


def forward(left, right, speed=255):
    left.set_speed(speed)
    right.set_speed(speed)

def backward(left, right, speed=255):
    left.set_speed(-speed)
    right.set_speed(-speed)

def turn_left(left, right, speed=255):
    left.set_speed(speed)
    right.set_speed(-speed)

def turn_right(left, right, speed=255):
    left.set_speed(-speed)
    right.set_speed(speed)

def bread(left, right):
    left.brake()
    right.brake()

def sleep(left, right, speed=0):
    left.set_speed(speed)
    right.set_speed(speed)

def main():
    left_motor = Motor(0, 0, 1)
    right_motor = Motor(4, 2, 3)

    print("前进")
    forward(left_motor, right_motor, 500)
    time.sleep(2)
    print("后退")
    backward(left_motor, right_motor, 500)
    time.sleep(2)
    print("左转")
    turn_left(left_motor, right_motor, 500)
    time.sleep(2)
    print("右转")
    turn_right(left_motor, right_motor, 500)
    time.sleep(2)
    print("前进")
    forward(left_motor, right_motor, 500)
    time.sleep(2)
    print("刹车")
    bread(left_motor, right_motor)
    time.sleep(2)
    print("前进")
    forward(left_motor, right_motor, 500)
    time.sleep(2)
    print("滑行")
    sleep(left_motor, right_motor)
    time.sleep(2)

if __name__ == "__main__":
    main()