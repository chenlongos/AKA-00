import threading
import time

from src.arm_control.interfaces import create_gripper
from src.base_control.interfaces import create_motor_pair
from src.base_control.pwm_channel_config import load_pwm_channels
from src.state import get_state_collector

_WHEEL_CIRCUMFERENCE_M = 3.1415926535 * 0.062  # 轮径 62mm


class ControlService:
    def __init__(self, config):
        self._config = config
        self._arm_driver = config.arm_driver
        self._duration_timer: threading.Timer | None = None
        self._duration_timer_lock = threading.Lock()
        self._arm_lock = threading.Lock()
        self._pwm_channels = load_pwm_channels(config)
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
        """闭环距离控制：编码器脉冲计数，到达目标停车（精度 ±2%）"""
        speed = min(100, max(1, abs(speed)))

        if direction == "forward":
            self._motor_pair.set_speed(speed, speed)
        elif direction == "backward":
            self._motor_pair.set_speed(-speed, -speed)
        elif direction == "left":
            self._motor_pair.set_speed(-speed, speed)
        elif direction == "right":
            self._motor_pair.set_speed(speed, -speed)
        else:
            raise ValueError(f"unknown direction: {direction}")

        # 编码器闭环：轮端 PPR=4680, 轮径 62mm, RPM 已含齿轮比
        pulses_per_mm = 4680.0 / (3.1415926535 * 62.0)
        target_pulses = int(distance_mm * pulses_per_mm)
        done_pulses = 0

        try:
            start_l, start_r = self._motor_pair.get_encoder()
        except (AttributeError, Exception):
            # 无编码器（MockMotorPair 等），回退到时间估算
            time.sleep(distance_mm / 1000.0 / 0.25)
            self._motor_pair.brake()
            return {"status": "success", "direction": direction,
                    "target_mm": distance_mm, "mode": "open_loop"}

        # 安全兜底：超时保护
        max_time = (distance_mm / 1000.0 / 0.1) * 3
        deadline = time.time() + max(1.0, max_time)

        # 接近目标时减速（防过冲）
        cruise_speed = speed
        crawl_speed = max(10, speed // 3)
        slow_at = int(target_pulses * 0.7)
        slowed = False

        while time.time() < deadline:
            try:
                cur_l, cur_r = self._motor_pair.get_encoder()
            except Exception:
                continue
            done_pulses = max(abs(cur_l - start_l), abs(cur_r - start_r))

            if not slowed and done_pulses >= slow_at:
                slowed = True
                s = crawl_speed
                sgn = 1 if direction in ("forward", "right") else -1
                if direction in ("forward", "backward"):
                    self._motor_pair.set_speed(sgn * s, sgn * s)
                else:
                    self._motor_pair.set_speed(-sgn * s, sgn * s)

            if done_pulses >= target_pulses:
                break

        time.sleep(0.02)  # 等串口清空
        self._motor_pair.brake()
        time.sleep(0.05)  # 等刹车生效
        return {"status": "success", "direction": direction,
                "target_mm": distance_mm,
                "traveled_mm": round(done_pulses / pulses_per_mm, 1)}

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

    def reinitialize_motor_pair(self) -> dict[str, object]:
        """重新初始化电机底盘（用于 tt_pid 等需要重置 ESP32 状态的场景）。"""
        reinit = getattr(self._motor_pair, "reinitialize", None)
        if reinit is not None:
            result = reinit()
            return {"status": "success", "reinitialize": result}
        return {"status": "success", "reinitialize": "not_supported"}

    def update_pwm_channels(self, pwm_channels: dict[str, int]) -> dict[str, object]:
        self._cancel_pending_stop()
        self._motor_pair.sleep()
        self._motor_pair.close()
        self._pwm_channels = pwm_channels.copy()
        self._motor_pair = self._create_motor_pair()
        get_state_collector().set_motor_pair(self._motor_pair)
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
