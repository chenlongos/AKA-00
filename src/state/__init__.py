from collections import deque
from dataclasses import asdict, dataclass
import threading
import time
from typing import Any, Callable, Optional

MAX_EXACT_DELTA_MS = 50
MAX_NEAREST_DELTA_MS = 100
MAX_INTERPOLATION_GAP_MS = 200
HISTORY_SIZE = 256


@dataclass
class MotorStatus:
    """左右轮状态管理"""
    left_speed: int = 0
    right_speed: int = 0
    left_target: int = 0
    right_target: int = 0


@dataclass
class MotorStatusRecord:
    timestamp_ms: int
    left_speed: int
    right_speed: int
    left_target: int
    right_target: int


class MotorStateTracker:
    """追踪电机状态，通过回调接收速度更新"""

    _instance: Optional['MotorStateTracker'] = None

    def __init__(self):
        self.status = MotorStatus()
        self._left_callback: Optional[Callable[[int], None]] = None
        self._right_callback: Optional[Callable[[int], None]] = None
        self._lock = threading.Lock()
        self._history: deque[MotorStatusRecord] = deque(maxlen=HISTORY_SIZE)
        self._record_snapshot()

    @classmethod
    def get_instance(cls) -> 'MotorStateTracker':
        if cls._instance is None:
            cls._instance = MotorStateTracker()
        return cls._instance

    def set_left_callback(self, callback: Callable[[int], None]):
        self._left_callback = callback

    def set_right_callback(self, callback: Callable[[int], None]):
        self._right_callback = callback

    def update_left(self, speed: int):
        with self._lock:
            self.status.left_speed = speed
            self._record_snapshot_locked()
        if self._left_callback:
            self._left_callback(speed)

    def update_right(self, speed: int):
        with self._lock:
            self.status.right_speed = speed
            self._record_snapshot_locked()
        if self._right_callback:
            self._right_callback(speed)

    def update_target(self, left: int, right: int):
        with self._lock:
            self.status.left_target = left
            self.status.right_target = right
            self._record_snapshot_locked()

    def get_status(self) -> MotorStatus:
        with self._lock:
            return MotorStatus(**asdict(self.status))

    def get_status_at(self, timestamp_ms: int) -> dict[str, Any]:
        with self._lock:
            records = list(self._history)
            current_status = MotorStatus(**asdict(self.status))

        if not records:
            return {
                "matched_timestamp_ms": timestamp_ms,
                "delta_ms": 0,
                "source": "current",
                "left_speed": current_status.left_speed,
                "right_speed": current_status.right_speed,
                "left_target": current_status.left_target,
                "right_target": current_status.right_target,
            }

        exact = min(records, key=lambda item: abs(item.timestamp_ms - timestamp_ms))
        exact_delta_ms = exact.timestamp_ms - timestamp_ms
        if abs(exact_delta_ms) <= MAX_EXACT_DELTA_MS:
            return self._build_point_response(timestamp_ms, exact, source="exact")

        prev_record = None
        next_record = None
        for record in records:
            if record.timestamp_ms <= timestamp_ms:
                prev_record = record
            if record.timestamp_ms >= timestamp_ms:
                next_record = record
                break

        if (
            prev_record is not None
            and next_record is not None
            and prev_record.timestamp_ms != next_record.timestamp_ms
        ):
            gap_ms = next_record.timestamp_ms - prev_record.timestamp_ms
            if gap_ms <= MAX_INTERPOLATION_GAP_MS:
                ratio = (timestamp_ms - prev_record.timestamp_ms) / gap_ms
                return {
                    "matched_timestamp_ms": timestamp_ms,
                    "delta_ms": 0,
                    "source": "interpolated",
                    "left_speed": prev_record.left_speed + (next_record.left_speed - prev_record.left_speed) * ratio,
                    "right_speed": prev_record.right_speed + (next_record.right_speed - prev_record.right_speed) * ratio,
                    "left_target": prev_record.left_target + (next_record.left_target - prev_record.left_target) * ratio,
                    "right_target": prev_record.right_target + (next_record.right_target - prev_record.right_target) * ratio,
                    "prev_timestamp_ms": prev_record.timestamp_ms,
                    "next_timestamp_ms": next_record.timestamp_ms,
                }

        if abs(exact_delta_ms) <= MAX_NEAREST_DELTA_MS:
            return self._build_point_response(timestamp_ms, exact, source="nearest")

        return self._build_point_response(timestamp_ms, exact, source="stale")

    def _build_point_response(self, query_timestamp_ms: int, record: MotorStatusRecord, *, source: str) -> dict[str, Any]:
        return {
            "matched_timestamp_ms": record.timestamp_ms,
            "delta_ms": record.timestamp_ms - query_timestamp_ms,
            "source": source,
            "left_speed": record.left_speed,
            "right_speed": record.right_speed,
            "left_target": record.left_target,
            "right_target": record.right_target,
        }

    def _record_snapshot(self) -> None:
        with self._lock:
            self._record_snapshot_locked()

    def _record_snapshot_locked(self) -> None:
        self._history.append(
            MotorStatusRecord(
                timestamp_ms=int(time.time() * 1000),
                left_speed=self.status.left_speed,
                right_speed=self.status.right_speed,
                left_target=self.status.left_target,
                right_target=self.status.right_target,
            )
        )
