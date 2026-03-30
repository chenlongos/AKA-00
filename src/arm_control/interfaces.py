"""舵机/夹爪硬件接口抽象。"""

from __future__ import annotations

import os
import sys
from typing import Literal, Optional, Protocol, runtime_checkable

GripperStatus = Literal["open", "closed", "moving", "unknown"]


@runtime_checkable
class ServoProtocol(Protocol):
    """单舵机抽象接口。"""

    def set_angle(self, servo_id: int, angle: float) -> None:
        ...

    def get_angle(self, servo_id: int) -> Optional[float]:
        ...


@runtime_checkable
class GripperProtocol(Protocol):
    """夹爪抽象接口。"""

    def open(self) -> None:
        ...

    def close(self) -> None:
        ...

    def get_status(self) -> GripperStatus:
        ...


class MockGripper:
    """Mock 夹爪，用于 Windows/macOS 开发。"""

    def __init__(self) -> None:
        self._status: GripperStatus = "unknown"

    def open(self) -> None:
        print("[MockGripper] open()")
        self._status = "open"

    def close(self) -> None:
        print("[MockGripper] close()")
        self._status = "closed"

    def get_status(self) -> GripperStatus:
        return self._status


class ZP10SGripperAdapter:
    """ZP10S 夹爪适配器。"""

    def __init__(self, zp10s) -> None:
        self._zp10s = zp10s
        self._status: GripperStatus = "unknown"

    def open(self) -> None:
        from src.arm_control.zl.zp10s.uart_control import release

        release(self._zp10s)
        self._status = "open"

    def close(self) -> None:
        from src.arm_control.zl.zp10s.uart_control import grab

        grab(self._zp10s)
        self._status = "closed"

    def get_status(self) -> GripperStatus:
        return self._status


class STS3215GripperAdapter:
    """STS3215 夹爪适配器。"""

    def __init__(self, servo) -> None:
        self._servo = servo

    def open(self) -> None:
        from src.arm_control.sts3215 import release

        release(self._servo)

    def close(self) -> None:
        from src.arm_control.sts3215 import grab1

        grab1(self._servo)

    def get_status(self) -> GripperStatus:
        position = self._servo.get_position(3)
        if position is None:
            return "unknown"
        if position > 3500:
            return "open"
        if position < 2800:
            return "closed"
        return "moving"


def create_gripper(
    driver: str = "zp10s",
    port: str = "/dev/ttyS2",
    baudrate: int = 115200,
) -> GripperProtocol:
    if os.name == "nt" or sys.platform == "darwin":
        return MockGripper()

    if driver == "zp10s":
        from src.arm_control.zl.zp10s.uart_control import ZP10S

        return ZP10SGripperAdapter(ZP10S(port, baudrate=baudrate))

    if driver == "sts3215":
        from src.arm_control.sts3215 import STS3215

        return STS3215GripperAdapter(STS3215(port, baudrate=baudrate))

    raise ValueError(f"unsupported arm driver: {driver}")
