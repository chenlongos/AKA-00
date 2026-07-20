"""电机硬件接口抽象。"""

import os
import sys
from typing import Protocol, runtime_checkable

from src.base_control.tt_pid import TtPidChassis


@runtime_checkable
class MotorPairProtocol(Protocol):
    """双轮底盘抽象接口。"""

    def set_speed(self, left: int, right: int) -> None:
        ...

    def get_speeds(self) -> tuple[int, int]:
        """获取左右轮实时 RPM。"""
        ...

    def brake(self) -> None:
        ...

    def sleep(self) -> None:
        ...

    def close(self) -> None:
        ...

    def reinitialize(self) -> bool:
        ...

    def get_encoder(self) -> tuple[int, int]:
        """读取编码器累计脉冲 (M1, M2)。"""
        ...


class MockMotorPair:
    """Mock 双轮底盘，用于 Windows/macOS 开发。"""

    def __init__(self) -> None:
        self._last_left = 0
        self._last_right = 0

    def set_speed(self, left: int, right: int) -> None:
        print(f"[MockMotorPair] set_speed(left={left}, right={right})")
        self._last_left = left
        self._last_right = right

    def get_speeds(self) -> tuple[int, int]:
        return self._last_left, self._last_right

    def brake(self) -> None:
        print("[MockMotorPair] brake()")
        self._last_left = 0
        self._last_right = 0

    def sleep(self) -> None:
        print("[MockMotorPair] sleep()")
        self._last_left = 0
        self._last_right = 0

    def close(self) -> None:
        pass

    def reinitialize(self) -> bool:
        return True

    def get_encoder(self) -> tuple[int, int]:
        return 0, 0


def create_motor_pair(
    port: str = "/dev/ttyS1",
    backend: str = "tt_pid",
) -> MockMotorPair | TtPidChassis:
    """创建双轮底盘。backend: tt_pid（ESP32 编码器） 或 mock（开发用）。"""
    if os.name == "nt" or sys.platform == "darwin":
        return MockMotorPair()

    if backend == "tt_pid":
        return TtPidChassis(port=port)

    return MockMotorPair()
