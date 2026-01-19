import time

from flask import Flask, request, jsonify, render_template

from motor import Motor, forward, backward, turn_left, turn_right, sleep, bread

left_motor = Motor(4, 0, 1)
right_motor = Motor(4, 2, 3)

app = Flask(__name__)

@app.route('/')
def index():
    return render_template('index.html')

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

    if milliseconds > 0 and action in ['up', 'down', 'left', 'right']:
        time.sleep(milliseconds / 1000.0)
        sleep(left_motor, right_motor)

        return jsonify({"status": "success", "message": f"{action} for {milliseconds}s done"})

    return jsonify({"status": "success", "action": action})

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=3000)