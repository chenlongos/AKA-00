import time

from src.arm_control.interfaces import create_gripper
from src.base_control.interfaces import create_motor_pair
from src.state import MotorStateTracker


class ControlService:
    def __init__(self, config):
        self._state_tracker = MotorStateTracker.get_instance()
        self._motor_pair = create_motor_pair(
            left_chip=config.base_left_chip,
            left_ch1=config.base_left_ch1,
            left_ch2=config.base_left_ch2,
            right_chip=config.base_right_chip,
            right_ch1=config.base_right_ch1,
            right_ch2=config.base_right_ch2,
            chip_type=config.base_chip_type,
        )
        self._gripper = create_gripper(
            driver=config.arm_driver,
            port=config.arm_port,
            baudrate=config.arm_baudrate,
        )

    def get_motor_status(self, timestamp: int) -> dict[str, int]:
        status = self._state_tracker.get_status()
        return {
            "timestamp": timestamp,
            "left_speed": status.left_speed,
            "right_speed": status.right_speed,
            "left_target": status.left_target,
            "right_target": status.right_target,
        }

    def execute_action(self, action: str, speed: int = 50, milliseconds: float = 0) -> dict:
        if not (self._apply_base_action(action, speed) or self._apply_arm_action(action)):
            raise ValueError(f"unsupported action: {action}")

        if milliseconds > 0 and action in ["up", "down", "left", "right"]:
            time.sleep(milliseconds / 1000.0)
            self._motor_pair.sleep()
            return {"status": "success", "message": f"{action} for {milliseconds}s done"}

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
        self._motor_pair.set_speed(left, right)
        if duration > 0:
            time.sleep(duration)
            self._motor_pair.sleep()
            return {"status": "success", "left": left, "right": right, "duration": duration}
        return {"status": "success", "left": left, "right": right}

    def send_raw_command(self, cmd: str) -> dict[str, str]:
        raw_sender = getattr(getattr(self._gripper, "_zp10s", None), "_send_raw_cmd", None)
        if cmd and raw_sender is not None:
            raw_sender(cmd)
        return {"status": "success", "cmd": cmd}

    def _apply_base_action(self, action: str, speed: int) -> bool:
        if action == "up":
            self._motor_pair.set_speed(speed, speed)
        elif action == "down":
            self._motor_pair.set_speed(-speed, -speed)
        elif action == "left":
            self._motor_pair.set_speed(-speed, speed)
        elif action == "right":
            self._motor_pair.set_speed(speed, -speed)
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
