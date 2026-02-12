import fcntl
import socket
import struct
import time

from flask import Blueprint, request, jsonify

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
        print('up')
        # forward(left_motor, right_motor, speed)
    elif action == 'down':
        print('down')
        # backward(left_motor, right_motor, speed)
    elif action == 'left':
        print('left')
        # turn_left(left_motor, right_motor, speed)
    elif action == 'right':
        print('right')
        # turn_right(left_motor, right_motor, speed)
    elif action == 'stop':
        print('stop')
        # brake(left_motor, right_motor)
    elif action == 'grab':
        print('grab')
        # grab(servo)
    elif action == 'release':
        print('release')
        # release(servo)

    if milliseconds > 0 and action in ['up', 'down', 'left', 'right']:
        time.sleep(milliseconds / 1000.0)
        # sleep(left_motor, right_motor)

        return jsonify({"status": "success", "message": f"{action} for {milliseconds}s done"})

    return jsonify({"status": "success", "action": action})

def get_ip(ifname="wlan0"):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    return socket.inet_ntoa(
        fcntl.ioctl(
            s.fileno(),
            0x8915,
            struct.pack('256s', ifname[:15].encode('utf-8'))
        )[20:24]
    )