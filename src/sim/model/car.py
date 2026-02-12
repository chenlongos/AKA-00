import math
import time

class CarModel:
    def __init__(self):
        self.x = 0.0
        self.y = 0.0
        self.theta = 0.0

        self.v = 0.0
        self.w = 0.0

        self.max_v = 2.0
        self.max_w = 2.0

        self.last_cmd_time = time.time()

    def set_cmd(self, v, w):
        self.v = max(min(v, self.max_v), -self.max_v)
        self.w = max(min(w, self.max_w), -self.max_w)
        self.last_cmd_time = time.time()

    def update(self, dt):
        if time.time() - self.last_cmd_time > 0.5:
            self.v = 0
            self.w = 0

        self.x += self.v * math.cos(self.theta) * dt
        self.y += self.v * math.sin(self.theta) * dt
        self.theta += self.w * dt

    def get_state(self):
        return {
            "x": self.x,
            "y": self.y,
            "theta": self.theta,
            "v": self.v,
            "w": self.w
        }
