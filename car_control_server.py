import fcntl
import socket
import struct
import threading
import time

from flask import Flask, request, jsonify, render_template

from arm import STS3215, grab, release, arm_init
from motor import Motor, forward, backward, turn_left, turn_right, sleep, bread

left_motor = Motor(4, 0, 1)
right_motor = Motor(4, 2, 3)

app = Flask(__name__)
servo = STS3215("/dev/ttyS2", baudrate=115200)
arm_init(servo)

def get_ip(ifname="wlan0"):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    return socket.inet_ntoa(
        fcntl.ioctl(
            s.fileno(),
            0x8915,
            struct.pack('256s', ifname[:15].encode('utf-8'))
        )[20:24]
    )


@app.route('/')
def index():
    return render_template('index.html')

@app.route("/ip")
def ip():
    return jsonify({
        "ip": get_ip()
    })

@app.route('/control', methods=['GET'])
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
        bread(left_motor, right_motor)
    elif action == 'grab':
        grab(servo)
    elif action == 'release':
        release(servo)

    if milliseconds > 0 and action in ['up', 'down', 'left', 'right']:
        time.sleep(milliseconds / 1000.0)
        sleep(left_motor, right_motor)

        return jsonify({"status": "success", "message": f"{action} for {milliseconds}s done"})

    return jsonify({"status": "success", "action": action})

def run_http():
    app.run(host='0.0.0.0', port=80)


def run_https():
    app.run(host='0.0.0.0', port=443, ssl_context=('/root/AKA-00/cert.pem', '/root/AKA-00/key.pem'))

if __name__ == '__main__':
    threading.Thread(target=run_http).start()
    threading.Thread(target=run_https).start()