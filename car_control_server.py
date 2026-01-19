import time

from flask import Flask, request, jsonify, app

from mock_motor import Motor, forward, backward, turn_left, turn_right, sleep

left_motor = Motor(4, 0, 1)
right_motor = Motor(4, 2, 3)

app = Flask(__name__)

@app.route('/control', methods=['GET'])
def control():
    action = request.args.get('action')
    speed = int(request.args.get('speed', 50))
    seconds = float(request.args.get('seconds', 2))

    # --- 运动逻辑 ---
    if action == 'up':
        forward(left_motor, right_motor, speed)
    elif action == 'down':
        backward(left_motor, right_motor, speed)
    elif action == 'left':
        turn_left(left_motor, right_motor, speed)
    elif action == 'right':
        turn_right(left_motor, right_motor, speed)

    if seconds > 0 and action in ['up', 'down', 'left', 'right']:
        time.sleep(seconds)
        sleep(left_motor, right_motor)

        return jsonify({"status": "success", "message": f"{action} for {seconds}s done"})

    return jsonify({"status": "success", "action": action})

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=3000)