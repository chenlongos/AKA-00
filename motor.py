import time
from periphery import PWM

# 配置常量
CHIP_CONFIGS = {
    'sg2002' : {'period_ns': 10000},
    'rk3588' : {'period_ns': 500000}
}

class Motor:
    def __init__(self, chip, ch1, ch2, chip_type='sg2002'):
        # 硬件初始化
        self.chip_type = chip_type
        self.period_ns = CHIP_CONFIGS[chip_type]['period_ns']

        if chip_type == 'sg2002':
            self.pwm1 = PWM(chip, ch1)
            self.pwm2 = PWM(chip, ch2)
        elif chip_type == 'rk3588':
            self.pwm1 = PWM(chip, 0)
            self.pwm2 = PWM(ch1, 0)
        else:
            raise ValueError(f"Unsupported chip type: {chip_type}")
        
        for p in (self.pwm1, self.pwm2):
            p.period_ns = self.period_ns
            p.duty_cycle_ns = self.period_ns // 2
            p.enable()

    def set_speed(self, speed):
        speed = max(-255, min(255, speed))
        duty = int(abs(speed) * self.period_ns // 255)
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
        duty = int(val * self.period_ns // 255)
        self.pwm1.duty_cycle_ns = duty
        self.pwm2.duty_cycle_ns = duty


def forward(left, right, speed=255):
    left.set_speed(speed)
    right.set_speed(speed)

def forward_left(left, right, speed=255):
    left.set_speed(speed)
    right.set_speed(speed * 0.8)

def forward_right(left, right, speed=255):
    left.set_speed(speed * 0.8)
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

def brake(left, right):
    left.brake()
    right.brake()

def sleep(left, right, speed=0):
    left.set_speed(speed)
    right.set_speed(speed)

def main():
    # sg2002
    # left_motor = Motor(0, 0, 1) 
    # right_motor = Motor(4, 2, 3) 
    # rk3588
    left_motor = Motor(0, 1, 0, chip_type='rk3588')      # (pin5, pin3)
    right_motor = Motor(4, 5, 0, chip_type='rk3588')     # (pin16, pin18)

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
    brake(left_motor, right_motor)
    time.sleep(2)
    print("前进")
    forward(left_motor, right_motor, 500)
    time.sleep(2)
    print("滑行")
    sleep(left_motor, right_motor)
    time.sleep(2)

if __name__ == "__main__":
    main()
