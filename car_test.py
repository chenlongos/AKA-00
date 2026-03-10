from src.arm_control.zl.zp10s.uart_control import ZP10S, grab_test
from src.base_control.n20.__init__ import N20, forward, backward, turn_left, turn_right, brake, sleep as n20_sleep
import time, subprocess

def arm_test():
    subprocess.run(['sh','./arm_init.sh'], capture_output=True, text=True)
    servo = ZP10S()
    print("开始机械臂测试")
    grab_test(servo)
    print("结束机械臂测试")

def moter_test():
    subprocess.run(['sh','./pwm_init.sh'], capture_output=True, text=True)
    left_motor = N20(4, 0, 1)
    right_motor = N20(4, 2, 3)
    print("开始电机测试")
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
    n20_sleep(left_motor, right_motor)
    time.sleep(2)
    print("结束电机测试")

if __name__ == "__main__":
    arm_test()
    time.sleep(1)
    moter_test()
