import os
import socket
import sys
try:
    import fcntl
    _HAS_FCNTL = True
except Exception:
    _HAS_FCNTL = False
import struct
import time

from flask import Blueprint, request, jsonify

# 硬件控制导入
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

left_motor = N20(4, 0, 1)
right_motor = N20(4, 2, 3)

servo = ZP10S("/dev/ttyS2", baudrate=115200)

api_bp = Blueprint("api", __name__)

@api_bp.route("/ip")
def ip():
    return jsonify({
        "ip": get_ip()
    })


@api_bp.route('/control', methods=['GET'])
def control():
    action = request.args.get('action')
    speed = int(request.args.get('speed', 50))
    milliseconds = float(request.args.get('time', 0))

    speed = speed * 240 // 50
    # --- 运动逻辑 ---
    if action == 'up':
        forward(left_motor, right_motor, speed)
    elif action == 'down':
        backward(left_motor, right_motor, speed)
    elif action == 'left':
        turn_left(left_motor, right_motor, speed)
    elif action == 'right':
        turn_right(left_motor, right_motor, speed)
    elif action == 'stop':
        brake(left_motor, right_motor)
    elif action == 'grab':
        grab(servo)
    elif action == 'release':
        release(servo)

    if milliseconds > 0 and action in ['up', 'down', 'left', 'right']:
        time.sleep(milliseconds / 1000.0)
        sleep(left_motor, right_motor)

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
