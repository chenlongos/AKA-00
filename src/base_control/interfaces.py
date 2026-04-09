"""电机硬件接口抽象。"""

from __future__ import annotations

import os
import sys
from typing import Protocol, runtime_checkable

from src.state import MotorStateTracker


@runtime_checkable
class MotorProtocol(Protocol):
    """单电机抽象接口。"""

    def set_speed(self, speed: int) -> None:
        ...

    def brake(self, val: int = 255) -> None:
        ...


@runtime_checkable
class MotorPairProtocol(Protocol):
    """双轮底盘抽象接口。"""

    def set_speed(self, left: int, right: int) -> None:
        ...

    def brake(self) -> None:
        ...

    def sleep(self) -> None:
        ...


class MotorPairAdapter:
    """N20 双轮适配器。"""

    def __init__(self, left: MotorProtocol, right: MotorProtocol) -> None:
        self._left = left
        self._right = right
        self._state_tracker = MotorStateTracker.get_instance()

    def set_speed(self, left: int, right: int) -> None:
        self._state_tracker.update_target(left, right)
        self._left.set_speed(left)
        self._right.set_speed(right)

    def brake(self) -> None:
        self._left.brake()
        self._right.brake()
        self._state_tracker.update_target(0, 0)

    def sleep(self) -> None:
        self._left.set_speed(0)
        self._right.set_speed(0)
        self._state_tracker.update_target(0, 0)


class MockMotorPair:
    """Mock 双轮底盘，用于 Windows/macOS 开发。"""

    def __init__(self) -> None:
        self._state_tracker = MotorStateTracker.get_instance()

    def set_speed(self, left: int, right: int) -> None:
        print(f"[MockMotorPair] set_speed(left={left}, right={right})")
        self._state_tracker.update_target(left, right)

    def brake(self) -> None:
        print("[MockMotorPair] brake()")
        self._state_tracker.update_target(0, 0)

    def sleep(self) -> None:
        print("[MockMotorPair] sleep()")
        self._state_tracker.update_target(0, 0)


def create_motor_pair(
    left_chip: int = 4,
    left_ch1: int = 0,
    left_ch2: int = 1,
    right_chip: int = 4,
    right_ch1: int = 2,
    right_ch2: int = 3,
    chip_type: str = "sg2002",
) -> MotorPairProtocol:
    if os.name == "nt" or sys.platform == "darwin":
        return MockMotorPair()

    from src.base_control.n20 import N20

    return MotorPairAdapter(
        N20(left_chip, left_ch1, left_ch2, chip_type=chip_type),
        N20(right_chip, right_ch1, right_ch2, chip_type=chip_type),
    )
