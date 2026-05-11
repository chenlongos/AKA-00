from dataclasses import dataclass
import threading
import time

import cv2

_WHEEL_CIRCUMFERENCE = 3.1415926535 * 0.065  # 轮子周长 (m)


@dataclass
class RobotStatus:
    """小车状态 - 用于ACT训练"""
    left_speed: float = 0.0
    right_speed: float = 0.0
    left_target: float = 0
    right_target: float = 0
    timestamp_ms: int = 0


class StateCollector:
    """状态采集线程，汇总 speed / action / camera"""

    _instance: "StateCollector | None" = None
    _lock = threading.Lock()

    def __init__(self):
        self._running = False
        self._thread: threading.Thread | None = None

        self._status = RobotStatus()
        self._data_lock = threading.Lock()

        # 相机和电机引用（延迟注入）
        self._camera = None
        self._motor_pair = None

        # JPEG 缓存（避免每次请求都编码）
        self._jpg_cache = None
        self._jpg_cache_time = 0.0
        self._jpg_lock = threading.Lock()
        self._encode_interval = 1.0 / 15  # 编码频率上限 10fps

    @classmethod
    def get_instance(cls) -> "StateCollector":
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = StateCollector()
        return cls._instance

    def set_motor_pair(self, motor_pair):
        self._motor_pair = motor_pair

    def set_camera(self, camera):
        self._camera = camera

    def clear_camera(self):
        self._camera = None

    def set_target_speed(self, left: int, right: int):
        with self._data_lock:
            self._status.left_target = left
            self._status.right_target = right

    def get_status(self) -> RobotStatus:
        return RobotStatus(
            left_speed=self._status.left_speed,
            right_speed=self._status.right_speed,
            left_target=self._status.left_target,
            right_target=self._status.right_target,
            timestamp_ms=int(time.time() * 1000),
        )

    def get_image(self):
        """返回缓存的 JPEG 图像（bytes），避免每次请求都编码"""
        with self._jpg_lock:
            return self._jpg_cache

    def _loop(self):
        """10fps 状态更新 + 图像编码"""
        interval = 1.0 / 10
        while self._running:
            start = time.time()

            left_speed, right_speed = 0.0, 0.0
            if self._motor_pair is not None:
                try:
                    left_rpm, right_rpm = self._motor_pair.get_speeds()
                    left_speed = round(left_rpm * _WHEEL_CIRCUMFERENCE / 60.0, 2)
                    right_speed = round(right_rpm * _WHEEL_CIRCUMFERENCE / 60.0, 2)
                except Exception:
                    pass

            with self._data_lock:
                self._status.left_speed = left_speed
                self._status.right_speed = right_speed

            # 图像编码（降低频率，避免每次请求都编码）
            now = time.time()
            if self._camera is not None and now - self._jpg_cache_time > self._encode_interval:
                ret, frame = self._camera.read()
                if ret and frame is not None:
                    ret, jpg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 20])
                    if ret:
                        with self._jpg_lock:
                            self._jpg_cache = jpg.tobytes()
                            self._jpg_cache_time = now

            elapsed = time.time() - start
            sleep_time = interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

    def start(self):
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=2)
            self._thread = None


def get_state_collector() -> StateCollector:
    return StateCollector.get_instance()