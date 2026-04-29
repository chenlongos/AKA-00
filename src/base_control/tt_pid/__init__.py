"""TT马达 ESP32-C3 底盘 UART 控制器。"""

import os
import struct
import time
from dataclasses import dataclass
from typing import Optional

import serial


# 协议常量
FRAME_H1 = 0xAA
FRAME_H2 = 0x55

CMD_INIT = 0x01
CMD_CONFIG = 0x02
CMD_SET_SPEED = 0x10
CMD_STOP = 0x11
CMD_BRAKE = 0x12
CMD_GET_RPM = 0x20
CMD_GET_STATUS = 0x21
CMD_RESET = 0xFF

RSP_ACK = 0x80
RSP_NACK = 0x81
RSP_RPM_DATA = 0x90
RSP_STATUS = 0x91


@dataclass
class RpmData:
    """左右轮 RPM 数据。"""
    left: int = 0
    right: int = 0


class TtPidChassis:
    """
    TT马达 ESP32-C3 底盘 UART 控制器。

    实现了 MotorPairProtocol 接口，可与 N20/Mock 互换。

    帧格式：0xAA 0x55 <cmd> <len> <payload...> <chk>
    校验：cmd ^ len ^ payload[0] ^ ... ^ payload[last]
    """

    def __init__(
        self,
        port: str = "/dev/ttyS2",
        baudrate: int = 115200,
        ppr: int = 4680,
        pwm_freq: int = 20000,
    ) -> None:
        self.ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.1,
        )
        time.sleep(0.5)
        self.ser.reset_input_buffer()

        if not self._init():
            raise RuntimeError("ESP32 init failed")
        if not self._config(ppr, pwm_freq):
            raise RuntimeError("ESP32 config failed")

    def close(self) -> None:
        if self.ser.is_open:
            self.ser.close()

    def _build_frame(self, cmd: int, payload: bytes = b"") -> bytes:
        chk = cmd ^ len(payload)
        for b in payload:
            chk ^= b
        return bytes([FRAME_H1, FRAME_H2, cmd, len(payload)]) + payload + bytes([chk])

    def _recv_frame(self, timeout: float = 0.2) -> Optional[dict]:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.ser.in_waiting < 4:
                time.sleep(0.01)
                continue
            h1 = self.ser.read(1)
            if not h1 or h1[0] != FRAME_H1:
                continue
            h2 = self.ser.read(1)
            if not h2 or h2[0] != FRAME_H2:
                continue
            header = self.ser.read(2)
            if len(header) < 2:
                continue
            cmd, length = header[0], header[1]
            payload = self.ser.read(length) if length else b""
            chk_b = self.ser.read(1)
            if not chk_b:
                continue
            chk = cmd ^ length
            for b in payload:
                chk ^= b
            if chk != chk_b[0]:
                return None
            return {"cmd": cmd, "payload": bytes(payload)}
        return None

    def _send_cmd(self, cmd: int, payload: bytes = b"", timeout: float = 0.2) -> Optional[dict]:
        self.ser.reset_input_buffer()
        self.ser.write(self._build_frame(cmd, payload))
        self.ser.flush()
        return self._recv_frame(timeout)

    def _init(self) -> bool:
        rsp = self._send_cmd(CMD_INIT)
        return rsp is not None and rsp["cmd"] == RSP_ACK

    def _config(self, ppr: int, pwm_freq: int) -> bool:
        payload = struct.pack(">HH", ppr, pwm_freq)
        rsp = self._send_cmd(CMD_CONFIG, payload)
        return rsp is not None and rsp["cmd"] == RSP_ACK

    def set_speed(self, left: int, right: int) -> None:
        """设置左右轮速度（-255 ~ 255）。"""
        left = max(-100, min(100, left))
        right = max(-100, min(100, right))
        left_sign = left // abs(left)
        right_sign = right // abs(right)
        left = (255 * 0.4) * (abs(left) / 100) + (255 * 0.6)
        left = max(-255, min(255, int(left)))
        right = (255 * 0.4) * (abs(right) / 100) + (255 * 0.6)
        right = max(-255, min(255, int(right)))
        self._set_motor_speed(0, left * left_sign)
        self._set_motor_speed(1, right * right_sign)

    def _set_motor_speed(self, motor_id: int, speed: int) -> bool:
        payload = struct.pack(">Bh", motor_id, speed)
        rsp = self._send_cmd(CMD_SET_SPEED, payload)
        return rsp is not None and rsp["cmd"] == RSP_ACK

    def brake(self) -> None:
        """刹车两个电机。"""
        self._brake_motor(0)
        self._brake_motor(1)

    def _brake_motor(self, motor_id: int) -> bool:
        rsp = self._send_cmd(CMD_BRAKE, bytes([motor_id]))
        return rsp is not None and rsp["cmd"] == RSP_ACK

    def sleep(self) -> None:
        """滑行停止两个电机。"""
        self._stop_motor(0)
        self._stop_motor(1)

    def _stop_motor(self, motor_id: int) -> bool:
        rsp = self._send_cmd(CMD_STOP, bytes([motor_id]))
        return rsp is not None and rsp["cmd"] == RSP_ACK

    def get_rpm(self) -> Optional[RpmData]:
        """获取左右轮 RPM。"""
        rsp = self._send_cmd(CMD_GET_RPM, bytes([2]))
        if rsp is None or rsp["cmd"] != RSP_RPM_DATA:
            return None
        payload = rsp["payload"]
        left, right = 0, 0
        i = 0
        while i + 2 < len(payload):
            mid = payload[i]
            rpm = struct.unpack(">h", payload[i + 1:i + 3])[0]
            if mid == 0:
                left = rpm
            elif mid == 1:
                right = rpm
            i += 3
        return RpmData(left=-left, right=right)

    def reset(self) -> bool:
        """重置控制器。"""
        rsp = self._send_cmd(CMD_RESET)
        return rsp is not None and rsp["cmd"] == RSP_ACK

    def get_speeds(self) -> tuple[int, int]:
        """获取左右轮实时速度（RPM）。"""
        rpm = self.get_rpm()
        if rpm is None:
            return 0, 0
        return rpm.left, rpm.right