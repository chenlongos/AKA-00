from dataclasses import dataclass
from typing import Callable, Optional


@dataclass
class MotorStatus:
    """左右轮状态管理"""
    left_speed: int = 0
    right_speed: int = 0


class MotorStateTracker:
    """追踪电机状态，通过回调接收速度更新"""

    _instance: Optional['MotorStateTracker'] = None

    def __init__(self):
        self.status = MotorStatus()
        self._left_callback: Optional[Callable[[int], None]] = None
        self._right_callback: Optional[Callable[[int], None]] = None

    @classmethod
    def get_instance(cls) -> 'MotorStateTracker':
        if cls._instance is None:
            cls._instance = MotorStateTracker()
        return cls._instance

    def set_left_callback(self, callback: Callable[[int], None]):
        self._left_callback = callback

    def set_right_callback(self, callback: Callable[[int], None]):
        self._right_callback = callback

    def update_left(self, speed: int):
        self.status.left_speed = speed
        if self._left_callback:
            self._left_callback(speed)

    def update_right(self, speed: int):
        self.status.right_speed = speed
        if self._right_callback:
            self._right_callback(speed)

    def get_status(self) -> MotorStatus:
        return self.status
