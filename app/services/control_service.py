import threading

from src.arm_control.interfaces import create_gripper
from src.base_control.interfaces import create_motor_pair
from src.base_control.pwm_channel_config import load_pwm_channels
from src.state import MotorStateTracker


class ControlService:
    def __init__(self, config):
        self._config = config
        self._arm_driver = config.arm_driver
        self._state_tracker = MotorStateTracker.get_instance()
        self._duration_timer: threading.Timer | None = None
        self._duration_timer_lock = threading.Lock()
        self._pwm_channels = load_pwm_channels(config)
        self._motor_pair = self._create_motor_pair()
        self._gripper = create_gripper(
            driver=config.arm_driver,
            port=config.arm_port,
            baudrate=config.arm_baudrate,
        )

    def _create_motor_pair(self):
        return create_motor_pair(
            left_chip=self._config.base_left_chip,
            left_ch1=self._pwm_channels["left_ch1"],
            left_ch2=self._pwm_channels["left_ch2"],
            right_chip=self._config.base_right_chip,
            right_ch1=self._pwm_channels["right_ch1"],
            right_ch2=self._pwm_channels["right_ch2"],
            chip_type=self._config.base_chip_type,
            backend=self._config.base_driver,
        )

    def get_motor_status(self, timestamp: int) -> dict[str, int]:
        return self._state_tracker.get_status_at(timestamp)

    def execute_action(self, action: str, speed: int = 50, milliseconds: float = 0) -> dict:
        self._cancel_pending_stop()
        if not (self._apply_base_action(action, speed) or self._apply_arm_action(action)):
            raise ValueError(f"unsupported action: {action}")

        if milliseconds > 0 and action in ["up", "down", "left", "right"]:
            self._schedule_stop(milliseconds / 1000.0)
            return {"status": "success", "message": f"{action} scheduled for {milliseconds}ms"}

        return {"status": "success", "action": action}

    def set_motor_speed(self, left: int, right: int) -> dict[str, int | str]:
        self._motor_pair.set_speed(left, right)
        return {"status": "success", "left": left, "right": right}

    def run_motor(self, left: int, right: int, duration: float = 0) -> dict[str, int | str]:
        """设置电机速度，可选持续时间

        Args:
            left: 左轮速度 (-100 ~ 100)
            right: 右轮速度 (-100 ~ 100)
            duration: 持续时间（秒），0 表示无限
        """
        self._cancel_pending_stop()
        self._motor_pair.set_speed(left, right)
        if duration > 0:
            self._schedule_stop(duration)
            return {"status": "success", "left": left, "right": right, "duration": duration, "mode": "scheduled"}
        return {"status": "success", "left": left, "right": right}

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

    def get_pwm_channels(self) -> dict[str, int]:
        return self._pwm_channels.copy()

    def update_pwm_channels(self, pwm_channels: dict[str, int]) -> dict[str, object]:
        self._cancel_pending_stop()
        self._motor_pair.sleep()
        self._motor_pair.close()
        self._pwm_channels = pwm_channels.copy()
        self._motor_pair = self._create_motor_pair()
        return {"status": "success", "pwm_channels": self.get_pwm_channels()}

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

    TURN_SPEED_RATIO = 0.3  # 转弯速度比例

    def _apply_base_action(self, action: str, speed: int) -> bool:
        if action == "up":
            self._motor_pair.set_speed(speed, speed)
        elif action == "down":
            self._motor_pair.set_speed(-speed, -speed)
        elif action == "left":
            self._motor_pair.set_speed(-int(speed * self.TURN_SPEED_RATIO), int(speed * self.TURN_SPEED_RATIO))
        elif action == "right":
            self._motor_pair.set_speed(int(speed * self.TURN_SPEED_RATIO), -int(speed * self.TURN_SPEED_RATIO))
        elif action == "stop":
            self._motor_pair.brake()
        else:
            return False
        return True

    def _apply_arm_action(self, action: str) -> bool:
        if action == "grab":
            self._gripper.close()
        elif action == "release":
            self._gripper.open()
        else:
            return False
        return True
