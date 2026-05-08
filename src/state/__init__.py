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

    # 轮胎参数（用于 RPM -> m/s 转换）
    _WHEEL_DIAMETER_M = 0.065  # 65mm

    def _rpm_to_mps(self, rpm: float) -> float:
        """将 RPM（转/分钟）转换为 m/s。"""
        circumference = 3.1415926535 * self._WHEEL_DIAMETER_M
        return rpm * circumference / 60.0

    def get_status_at(self, timestamp_ms: int) -> dict[str, Any]:
        current_timestamp_ms = int(time.time() * 1000)

        left_speed, right_speed = 0, 0
        left_target, right_target = 0, 0

        if self._motor_pair is not None:
            left_rpm, right_rpm = self._motor_pair.get_speeds()
            # 转换为 m/s，保留两位小数
            left_speed = round(self._rpm_to_mps(left_rpm), 2)
            right_speed = round(self._rpm_to_mps(right_rpm), 2)

        return {
            "matched_timestamp_ms": current_timestamp_ms,
            "delta_ms": current_timestamp_ms - timestamp_ms,
            "source": "current",
            "left_speed": left_speed,
            "right_speed": right_speed,
            "left_target": left_target,
            "right_target": right_target,
        }
