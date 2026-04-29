from dataclasses import dataclass
import time
from typing import Any


@dataclass
class RobotStatus:
    """左右轮状态管理"""
    left_speed: float = 0.0
    right_speed: float = 0.0
    left_target: int = 0
    right_target: int = 0


class MotorStateTracker:
    """追踪电机状态"""

    _instance: 'MotorStateTracker | None' = None

    def __init__(self):
        self._motor_pair = None
        self._last_update_ms = int(time.time() * 1000)

    @classmethod
    def get_instance(cls) -> 'MotorStateTracker':
        if cls._instance is None:
            cls._instance = MotorStateTracker()
        return cls._instance

    def set_motor_pair(self, motor_pair):
        """注入 motor pair 引用，由 ControlService 调用"""
        self._motor_pair = motor_pair

    def get_status_at(self, timestamp_ms: int) -> dict[str, Any]:
        current_timestamp_ms = int(time.time() * 1000)

        left_speed, right_speed = 0, 0
        left_target, right_target = 0, 0

        if self._motor_pair is not None:
            left_speed, right_speed = self._motor_pair.get_speeds()

        return {
            "matched_timestamp_ms": current_timestamp_ms,
            "delta_ms": current_timestamp_ms - timestamp_ms,
            "source": "current",
            "left_speed": left_speed,
            "right_speed": right_speed,
            "left_target": left_target,
            "right_target": right_target,
        }
