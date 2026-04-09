from collections import deque
from dataclasses import asdict, dataclass
import math
import threading
import time
from typing import Any, Callable, Optional

MAX_EXACT_DELTA_MS = 50
MAX_NEAREST_DELTA_MS = 100
MAX_INTERPOLATION_GAP_MS = 200
HISTORY_SIZE = 256
MOTOR_RESPONSE_TIME_MS = 180.0


@dataclass
class MotorStatus:
    """左右轮状态管理"""
    left_speed: float = 0.0
    right_speed: float = 0.0
    left_target: int = 0
    right_target: int = 0


@dataclass
class MotorStatusRecord:
    timestamp_ms: int
    left_speed: float
    right_speed: float
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
        self._last_update_ms = int(time.time() * 1000)
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
            self._advance_to_locked(int(time.time() * 1000))
            self.status.left_speed = speed
            self._record_snapshot_locked()
        if self._left_callback:
            self._left_callback(speed)

    def update_right(self, speed: int):
        with self._lock:
            self._advance_to_locked(int(time.time() * 1000))
            self.status.right_speed = speed
            self._record_snapshot_locked()
        if self._right_callback:
            self._right_callback(speed)

    def update_target(self, left: int, right: int):
        with self._lock:
            self._advance_to_locked(int(time.time() * 1000))
            self.status.left_target = left
            self.status.right_target = right
            self._record_snapshot_locked()

    def get_status(self) -> MotorStatus:
        with self._lock:
            self._advance_to_locked(int(time.time() * 1000))
            return MotorStatus(**asdict(self.status))

    def get_status_at(self, timestamp_ms: int) -> dict[str, Any]:
        with self._lock:
            self._advance_to_locked(int(time.time() * 1000))
            records = list(self._history)
            current_status = MotorStatus(**asdict(self.status))
            current_timestamp_ms = self._last_update_ms

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

        if timestamp_ms >= current_timestamp_ms:
            predicted = self._advance_status(current_status, current_timestamp_ms, timestamp_ms)
            return self._build_status_response(timestamp_ms, timestamp_ms, predicted, source="predicted")

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
                estimated = self._advance_record(prev_record, timestamp_ms)
                payload = self._build_status_response(
                    timestamp_ms,
                    timestamp_ms,
                    estimated,
                    source="interpolated",
                )
                payload["prev_timestamp_ms"] = prev_record.timestamp_ms
                payload["next_timestamp_ms"] = next_record.timestamp_ms
                return payload

        if abs(exact_delta_ms) <= MAX_NEAREST_DELTA_MS:
            return self._build_point_response(timestamp_ms, exact, source="nearest")

        reference = prev_record if prev_record is not None else exact
        return self._build_status_response(
            timestamp_ms,
            timestamp_ms,
            self._advance_record(reference, timestamp_ms),
            source="stale",
        )

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

    def _build_status_response(
        self,
        query_timestamp_ms: int,
        matched_timestamp_ms: int,
        status: MotorStatus,
        *,
        source: str,
    ) -> dict[str, Any]:
        return {
            "matched_timestamp_ms": matched_timestamp_ms,
            "delta_ms": matched_timestamp_ms - query_timestamp_ms,
            "source": source,
            "left_speed": status.left_speed,
            "right_speed": status.right_speed,
            "left_target": status.left_target,
            "right_target": status.right_target,
        }

    def _record_snapshot(self) -> None:
        with self._lock:
            self._record_snapshot_locked()

    def _advance_to_locked(self, timestamp_ms: int) -> None:
        if timestamp_ms <= self._last_update_ms:
            return
        self.status = self._advance_status(self.status, self._last_update_ms, timestamp_ms)
        self._last_update_ms = timestamp_ms

    def _advance_record(self, record: MotorStatusRecord, timestamp_ms: int) -> MotorStatus:
        return self._advance_status(
            MotorStatus(
                left_speed=record.left_speed,
                right_speed=record.right_speed,
                left_target=record.left_target,
                right_target=record.right_target,
            ),
            record.timestamp_ms,
            timestamp_ms,
        )

    def _advance_status(self, status: MotorStatus, from_timestamp_ms: int, to_timestamp_ms: int) -> MotorStatus:
        delta_ms = max(to_timestamp_ms - from_timestamp_ms, 0)
        if delta_ms == 0:
            return MotorStatus(**asdict(status))
        alpha = 1.0 - math.exp(-delta_ms / MOTOR_RESPONSE_TIME_MS)
        return MotorStatus(
            left_speed=status.left_speed + (status.left_target - status.left_speed) * alpha,
            right_speed=status.right_speed + (status.right_target - status.right_speed) * alpha,
            left_target=status.left_target,
            right_target=status.right_target,
        )

    def _record_snapshot_locked(self) -> None:
        self._history.append(
            MotorStatusRecord(
                timestamp_ms=self._last_update_ms,
                left_speed=self.status.left_speed,
                right_speed=self.status.right_speed,
                left_target=self.status.left_target,
                right_target=self.status.right_target,
            )
        )
