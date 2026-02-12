import math


class CarModel:
    def __init__(self):
        self.x = 0.0
        self.y = 0.0
        self.angle = -math.pi / 2
        self.speed = 0.0
        self.acceleration = 0.2
        self.maxSpeed = 5
        self.friction = 0.95
        self.rotationSpeed = 0.05

    def reset(self):
        self.x = 0.0
        self.y = 0.0
        self.angle = -math.pi / 2
        self.speed = 0.0

    def get_state(self):
        return {
            "x": self.x,
            "y": self.y,
            "angle": self.angle,
            "speed": self.speed,
            "acceleration": self.acceleration,
            "maxSpeed": self.maxSpeed,
            "friction": self.friction,
            "rotationSpeed": self.rotationSpeed,
        }


car = CarModel()
