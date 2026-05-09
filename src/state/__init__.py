from dataclasses import dataclass
import threading
import time

@dataclass
class RobotStatus:
    """左右轮状态管理"""
    left_speed: float = 0.0
    right_speed: float = 0.0
    left_target: int = 0
    right_target: int = 0


class StateCollector:
    """15fps 状态采集线程，汇总 action / image / state"""

    _instance: "StateCollector | None" = None
    _lock = threading.Lock()

    def __init__(self):
        self._running = False
        self._thread: threading.Thread | None = None

        # 最新数据（线程安全）
        self._action = {"left": 0, "right": 0}
        self._image = None
        self._state = {"left_speed": 0.0, "right_speed": 0.0}
        self._data_lock = threading.Lock()

        # 相机和电机引用（延迟注入）
        self._camera = None
        self._motor_pair = None

    @classmethod
    def get_instance(cls) -> "StateCollector":
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = StateCollector()
        return cls._instance

    def set_camera(self, camera):
        self._camera = camera

    def set_motor_pair(self, motor_pair):
        self._motor_pair = motor_pair

    def set_action(self, left: int, right: int):
        with self._data_lock:
            self._action = {"left": left, "right": right}

    def get_action(self) -> dict:
        with self._data_lock:
            return self._action.copy()

    def get_image(self):
        with self._data_lock:
            return self._image

    def get_state(self) -> dict:
        with self._data_lock:
            return self._state.copy()

    def _update(self):
        """采集最新数据（每帧调用一次）"""
        current_ms = int(time.time() * 1000)

        # 1. 更新 image
        if self._camera is not None:
            ret, frame = self._camera.read()
            if ret:
                with self._data_lock:
                    self._image = frame

        # 2. 更新 state
        left_speed, right_speed = 0.0, 0.0
        if self._motor_pair is not None:
            left_rpm, right_rpm = self._motor_pair.get_speeds()
            circumference = 3.1415926535 * 0.065
            left_speed = round(left_rpm * circumference / 60.0, 2)
            right_speed = round(right_rpm * circumference / 60.0, 2)

        with self._data_lock:
            self._state = {
                "left_speed": left_speed,
                "right_speed": right_speed,
                "timestamp_ms": current_ms,
            }

    def _loop(self):
        interval = 1.0 / 15  # 15fps
        while self._running:
            start = time.time()
            self._update()
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
