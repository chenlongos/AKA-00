import os
try:
    import fcntl
    _HAS_FCNTL = True
except Exception:
    _HAS_FCNTL = False
import socket
import struct
import time

from flask import Blueprint, request, jsonify

if os.name == "nt":
    class STS3215:
        def __init__(self, *_, **__):
            pass

    def grab(_):
        return None

    def release(_):
        return None

    def arm_init(_):
        return None

    class Motor:
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
    from arm import STS3215, grab, release, arm_init
    from motor import Motor, forward, backward, turn_left, turn_right, sleep, brake

left_motor = Motor(4, 0, 1)
right_motor = Motor(4, 2, 3)

servo = STS3215("/dev/ttyS2", baudrate=115200)
arm_init(servo)

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
        # print('up')
        forward(left_motor, right_motor, speed)
    elif action == 'down':
        # print('down')
        backward(left_motor, right_motor, speed)
    elif action == 'left':
        # print('left')
        turn_left(left_motor, right_motor, speed)
    elif action == 'right':
        # print('right')
        turn_right(left_motor, right_motor, speed)
    elif action == 'stop':
        # print('stop')
        brake(left_motor, right_motor)
    elif action == 'grab':
        # print('grab')
        grab(servo)
    elif action == 'release':
        # print('release')
        release(servo)

    if milliseconds > 0 and action in ['up', 'down', 'left', 'right']:
        time.sleep(milliseconds / 1000.0)
        # sleep(left_motor, right_motor)

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
