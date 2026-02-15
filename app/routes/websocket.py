import math

from src.sim.model.car import car
from ..extensions import socketio
from flask_socketio import emit


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
