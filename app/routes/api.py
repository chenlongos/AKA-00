import socket
import struct

try:
    import fcntl
    _HAS_FCNTL = True
except Exception:
    _HAS_FCNTL = False

from flask import Blueprint, request, jsonify

# 硬件控制导入
import os
import sys

if os.name == "nt" or sys.platform == "darwin":
    class ZP10S:
        def __init__(self, *_, **__):
            pass

    def grab(_):
        return None

    def release(_):
        return None

    def arm_init(_):
        return None

    class N20:
        def __init__(self, *_, **__):
            pass

        def set_speed(self, _):
            pass

    def forward(*_, **__):
        return None

    def backward(*_, **__):
        return None

    def turn_left(*_, **__):
        return None

    def turn_right(*_, **__):
        return None

    def sleep(*_, **__):
        return None

    def brake(*_, **__):
        return None
else:
    from src.arm_control.zl.zp10s.uart_control import ZP10S, grab, release
    from src.base_control.n20 import N20, forward, backward, turn_left, turn_right, brake
    from src.state import MotorStateTracker

left_motor = N20(4, 0, 1)
right_motor = N20(4, 2, 3)

# 初始化状态追踪器
state_tracker = MotorStateTracker.get_instance()

servo = ZP10S("/dev/ttyS2", baudrate=115200)

api_bp = Blueprint("api", __name__)


@api_bp.route("/ip")
def ip():
    return jsonify({
        "ip": get_ip()
    })


@api_bp.route("/motor_status")
def motor_status():
    """获取小车左右轮当前状态"""
    status = state_tracker.get_status()
    return jsonify({
        "left_speed": status.left_speed,
        "right_speed": status.right_speed
    })


@api_bp.route('/control', methods=['GET'])
def control():
    import time
    action = request.args.get('action')
    speed = int(request.args.get('speed', 50))
    milliseconds = float(request.args.get('time', 0))

    speed = speed * 240 // 50
    # --- 运动逻辑 ---
    if action == 'up':
        forward(left_motor, right_motor, speed)
        state_tracker.update_left(speed)
        state_tracker.update_right(speed)
    elif action == 'down':
        backward(left_motor, right_motor, speed)
        state_tracker.update_left(-speed)
        state_tracker.update_right(-speed)
    elif action == 'left':
        turn_left(left_motor, right_motor, speed)
        state_tracker.update_left(speed)
        state_tracker.update_right(-speed)
    elif action == 'right':
        turn_right(left_motor, right_motor, speed)
        state_tracker.update_left(-speed)
        state_tracker.update_right(speed)
    elif action == 'stop':
        brake(left_motor, right_motor)
        state_tracker.update_left(0)
        state_tracker.update_right(0)
    elif action == 'grab':
        grab(servo)
    elif action == 'release':
        release(servo)

    if milliseconds > 0 and action in ['up', 'down', 'left', 'right']:
        time.sleep(milliseconds / 1000.0)
        sleep(left_motor, right_motor)
        state_tracker.update_left(0)
        state_tracker.update_right(0)

        return jsonify({"status": "success", "message": f"{action} for {milliseconds}s done"})

    return jsonify({"status": "success", "action": action})


def get_ip(ifname="wlan0"):
    if not _HAS_FCNTL:
        return socket.gethostbyname(socket.gethostname())
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    return socket.inet_ntoa(
        fcntl.ioctl(
            s.fileno(),
            0x8915,
            struct.pack('256s', ifname[:15].encode('utf-8'))
        )[20:24]
    )

@api_bp.route('/raw_command', methods=['GET'])
def raw_command():
    cmd = request.args.get('cmd', '')
    if cmd:
        servo._send_raw_cmd(cmd)
    return jsonify({"status": "success", "cmd": cmd})


@api_bp.route('/motor_direct', methods=['GET'])
def motor_direct():
    """直接设置左右轮速度

    Query params:
        left: int  左轮速度 (-255 ~ 255)
        right: int 右轮速度 (-255 ~ 255)
    """
    left = int(request.args.get('left', 0))
    right = int(request.args.get('right', 0))

    left_motor.set_speed(left)
    right_motor.set_speed(right)
    state_tracker.update_left(left)
    state_tracker.update_right(right)

    return jsonify({"status": "success", "left": left, "right": right})