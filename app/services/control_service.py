import threading
import time

from src.arm_control.interfaces import create_gripper
from src.base_control.interfaces import create_motor_pair
from src.state import get_state_collector

_WHEEL_CIRCUMFERENCE_M = 3.1415926535 * 0.062  # 轮径 62mm


class ControlService:
    def __init__(self, config):
        self._config = config
        self._arm_driver = config.arm_driver
        self._duration_timer: threading.Timer | None = None
        self._duration_timer_lock = threading.Lock()
        self._arm_lock = threading.Lock()
        self._motor_pair = self._create_motor_pair()
        self._gripper = create_gripper(
            driver=config.arm_driver,
            port=config.arm_port,
            baudrate=config.arm_baudrate,
        )

    def _create_motor_pair(self):
        motor_pair = create_motor_pair(
            port=self._config.base_port,
            backend=self._config.base_driver,
        )
        get_state_collector().set_motor_pair(motor_pair)
        return motor_pair

    def execute_action(self, action: str, speed: int = 50, milliseconds: float = 0) -> dict:
        self._cancel_pending_stop()
        if not (self._apply_base_action(action, speed) or self._apply_arm_action(action)):
            raise ValueError(f"unsupported action: {action}")

        if milliseconds > 0 and action in ["up", "down", "left", "right"]:
            self._schedule_stop(milliseconds / 1000.0)
            return {"status": "success", "message": f"{action} scheduled for {milliseconds}ms"}

        return {"status": "success", "action": action}

    def run_motor(self, left: int, right: int, duration: float = 0) -> dict[str, int | str]:
        """设置电机速度，可选持续时间

        Args:
            left: 左轮速度 (-100 ~ 100)
            right: 右轮速度 (-100 ~ 100)
            duration: 持续时间（秒），0 表示无限
        """
        self._cancel_pending_stop()
        self._motor_pair.set_speed(left, right)
        get_state_collector().set_target_speed(left, right)
        if duration > 0:
            self._schedule_stop(duration)
            return {"status": "success", "left": left, "right": right, "duration": duration, "mode": "scheduled"}
        return {"status": "success", "left": left, "right": right}

    def move_distance(self, direction: str, distance_mm: float, speed: int) -> dict:
        """ESP32 MCU 内部执行距离控制。"""
        import struct
        speed = min(100, max(1, abs(speed)))
        pulses_per_mm = 4680.0 / (3.1415926535 * 62.0)
        target = int(distance_mm * pulses_per_mm)
        dir_map = {"forward": 0, "backward": 1, "left": 2, "right": 3}

        if direction not in dir_map:
            raise ValueError(f"unknown direction: {direction}")

        try:
            payload = struct.pack(">BBi", dir_map[direction], speed, target)
            self._motor_pair._send_cmd_noresp(0x23, payload)
        except Exception:
            return {"status": "error", "message": "CMD_MOVE_DISTANCE failed"}

        return {"status": "started", "mode": "esp32", "target_mm": distance_mm}

    def send_raw_command(self, cmd: str) -> dict[str, str]:
        raw_sender = getattr(getattr(self._gripper, "_zp10s", None), "_send_raw_cmd", None)
        if cmd and raw_sender is not None:
            raw_sender(cmd)
        return {"status": "success", "cmd": cmd}

    def update_arm_angles(self, driver: str, angles: dict[str, int]) -> dict[str, object]:
        if driver != self._arm_driver:
            raise ValueError(f"driver mismatch: expected {self._arm_driver}, got {driver}")
        updater = getattr(self._gripper, "update_angles", None)
        if updater is not None:
            updater(angles)
        return {"status": "success", "driver": driver, "angles": angles}

    def preview_arm_angle(self, driver: str, key: str, angle: int) -> dict[str, object]:
        if driver != self._arm_driver:
            raise ValueError(f"driver mismatch: expected {self._arm_driver}, got {driver}")
        previewer = getattr(self._gripper, "preview_angle", None)
        if previewer is None:
            raise ValueError("current gripper does not support angle preview")
        previewer(key, angle)
        return {"status": "success", "driver": driver, "key": key, "angle": angle}

    def reinitialize_motor_pair(self) -> dict[str, object]:
        """重新初始化电机底盘（用于 tt_pid 等需要重置 ESP32 状态的场景）。"""
        reinit = getattr(self._motor_pair, "reinitialize", None)
        if reinit is not None:
            result = reinit()
            return {"status": "success", "reinitialize": result}
        return {"status": "success", "reinitialize": "not_supported"}

    def _cancel_pending_stop(self) -> None:
        with self._duration_timer_lock:
            if self._duration_timer is not None:
                self._duration_timer.cancel()
                self._duration_timer = None

    def _schedule_stop(self, duration: float) -> None:
        timer = threading.Timer(duration, self._stop_motors)
        timer.daemon = True
        with self._duration_timer_lock:
            if self._duration_timer is not None:
                self._duration_timer.cancel()
            self._duration_timer = timer
        timer.start()

    def _stop_motors(self) -> None:
        self._motor_pair.sleep()
        with self._duration_timer_lock:
            self._duration_timer = None

    def _apply_base_action(self, action: str, speed: int) -> bool:
        if action == "up":
            self._motor_pair.set_speed(speed, speed)
            get_state_collector().set_target_speed(speed, speed)
        elif action == "down":
            self._motor_pair.set_speed(-speed, -speed)
            get_state_collector().set_target_speed(-speed, -speed)
        elif action == "left":
            self._motor_pair.set_speed(-speed, speed)
            get_state_collector().set_target_speed(-speed, speed)
        elif action == "right":
            self._motor_pair.set_speed(speed, -speed)
            get_state_collector().set_target_speed(speed, -speed)
        elif action == "stop":
            self._motor_pair.brake()
            get_state_collector().set_target_speed(0, 0)
        else:
            return False
        return True

    def _apply_arm_action(self, action: str) -> bool:
        collector = get_state_collector()
        if action == "grab":
            collector.set_gripper_target(1)
            collector.set_gripper_status("closed")
            threading.Thread(target=self._do_grab, daemon=True).start()
        elif action == "release":
            threading.Thread(target=self._do_release, daemon=True).start()
        else:
            return False
        return True

    def _do_grab(self):
        with self._arm_lock:
            self._gripper.close()
        get_state_collector().set_gripper_target(0)

    def _do_release(self):
        with self._arm_lock:
            self._gripper.open()
