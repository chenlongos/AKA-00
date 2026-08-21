"""舵机/夹爪硬件接口抽象。"""

from __future__ import annotations

import os
import re
import sys
from typing import Any, Literal, Optional, Protocol, runtime_checkable

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

    def disconnect(self) -> None:
        """断开底层串口连接，把硬件让渡给外部程序。"""
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

    def update_angles(self, angles: dict[str, Any]) -> None:
        pass

    def preview_angle(self, key: str, angle: int) -> None:
        print(f"[MockGripper] preview_angle({key}={angle})")

    def disconnect(self) -> None:
        pass


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

    def update_angles(self, angles: dict[str, Any]) -> None:
        self._zp10s.update_angles(angles)

    def preview_angle(self, key: str, angle: int) -> None:
        servo_id = _resolve_servo_id(key, gripper_servo=2)
        self._zp10s.set_angle(servo_id, angle)

    def disconnect(self) -> None:
        self._zp10s.close()


class STS3215GripperAdapter:
    """STS3215 夹爪适配器。"""

    def __init__(self, servo) -> None:
        self._servo = servo

    def open(self) -> None:
        from src.arm_control.sts3215 import release

        release(self._servo)

    def close(self) -> None:
        from src.arm_control.sts3215 import grab

        grab(self._servo)

    def get_status(self) -> GripperStatus:
        position = self._servo.get_position(3)
        if position is None:
            return "unknown"
        if position > 3500:
            return "open"
        if position < 2800:
            return "closed"
        return "moving"

    def update_angles(self, angles: dict[str, Any]) -> None:
        self._servo.update_angles(angles)

    def preview_angle(self, key: str, angle: int) -> None:
        servo_id = _resolve_servo_id(key, gripper_servo=3)
        self._servo.move_to_position(servo_id, angle)

    def disconnect(self) -> None:
        self._servo.close()


def _extract_servo_id(key: str) -> int:
    """从 key 中提取舵机 ID。

    支持两种格式:
      - 旧格式: "servo0_prepare" → 0
      - 新格式: "grab_position.servo1" → 1, "lift_position.servo2" → 2
    不处理 gripper_open / gripper_close（无 servo ID）。
    """
    # 新格式: "xxx.servoN"
    match = re.search(r"\.servo(\d+)$", key)
    if match:
        return int(match.group(1))
    # 旧格式: "servoN_..."
    match = re.match(r"servo(\d+)_", key)
    if match:
        return int(match.group(1))
    # 纯 "servoN"
    match = re.match(r"^servo(\d+)$", key)
    if match:
        return int(match.group(1))
    raise ValueError(f"invalid servo key: {key}")


def _resolve_servo_id(key: str, gripper_servo: int) -> int:
    """解析 servo ID，对 gripper_open/gripper_close 使用驱动对应的夹爪舵机 ID。"""
    if key in ("gripper_open", "gripper_close"):
        return gripper_servo
    return _extract_servo_id(key)


def create_gripper(
    driver: str = "zp10s",
    port: str = "/dev/ttyS10",
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
