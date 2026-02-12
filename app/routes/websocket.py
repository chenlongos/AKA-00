import math

from src.sim.model.car import car
from ..extensions import socketio
from flask_socketio import emit


# 监听客户端发送的 'message' 事件
@socketio.on('message')
def handle_message(data):
    print('收到消息: ' + data)
    # 将消息回传给发送者
    emit('response', {'data': '服务器已收到: ' + data})


# 监听自定义事件 'my_event'
@socketio.on('my_event')
def handle_my_custom_event(json):
    print('收到自定义事件数据: ' + str(json))
    # broadcast=True 表示广播给所有连接的客户端（群发）
    emit('my_response', json, broadcast=True)


@socketio.on('action')
def handle_action(action):
    if action == 'up':
        if car.speed < car.maxSpeed:
            car.speed += car.acceleration
    if action == 'down':
        if car.speed > -car.maxSpeed / 2:
            car.speed -= car.acceleration
    if action == 'left':
        car.angle -= car.rotationSpeed
    if action == 'right':
        car.angle += car.rotationSpeed

    car.speed *= car.friction
    car.x += math.cos(car.angle) * car.speed
    car.y += math.sin(car.angle) * car.speed

    if action == 'stop':
        car.x -= math.cos(car.angle) * car.speed * 2
        car.y -= math.sin(car.angle) * car.speed * 2
        car.speed = 0
    state = car.get_state()
    emit('car_state', state, broadcast=True)


@socketio.on('get_car_state')
def get_car_state():
    status = car.get_state()
    emit('car_state', status, broadcast=True)


@socketio.on('reset_car_state')
def get_car_state():
    car.reset()
    status = car.get_state()
    emit('car_state', status, broadcast=True)
