from __future__ import annotations

import json
from pathlib import Path

ZP10S_DRIVER = "zp10s"
STS3215_DRIVER = "sts3215"

DEFAULT_ZP10S_ARM_ANGLES = {
    "servo0_prepare": 245,
    "servo1_prepare": 180,
    "servo2_prepare": 150,
    "servo2_approach": 150,
    "servo2_grab": 90,
    "servo0_lift": 200,
    "servo1_lift": 180,
    "servo2_lift": 90,
}

DEFAULT_STS3215_ARM_ANGLES = {
    "servo1_prepare": 2300,
    "servo2_prepare": 2100,
    "servo3_prepare": 4000,
    "servo1_enter": 1850,
    "servo2_enter": 2650,
    "servo3_enter": 4000,
    "servo3_grab": 3000,
    "servo1_lift": 2300,
    "servo2_lift": 2100,
    "servo3_lift": 3000,
}

_DEFAULTS_BY_DRIVER = {
    ZP10S_DRIVER: DEFAULT_ZP10S_ARM_ANGLES,
    STS3215_DRIVER: DEFAULT_STS3215_ARM_ANGLES,
}

_ARM_ANGLES_PATH = Path(__file__).resolve().parents[2] / "arm_angles.json"


def get_arm_angles_path() -> Path:
    return _ARM_ANGLES_PATH


def load_arm_angles(driver: str) -> dict[str, int]:
    defaults = _get_defaults(driver)
    raw_data = _read_arm_angles_file()
    if isinstance(raw_data.get(driver), dict):
        source = raw_data[driver]
    elif driver == ZP10S_DRIVER:
        source = raw_data
    else:
        source = {}
    return _normalize_angles(source, defaults)


def save_arm_angles(driver: str, angles: dict[str, object]) -> dict[str, int]:
    defaults = _get_defaults(driver)
    normalized = _normalize_angles(angles, defaults)
    raw_data = _read_arm_angles_file()

    if driver == ZP10S_DRIVER and not any(isinstance(value, dict) for value in raw_data.values()):
        data_to_write: dict[str, object] = normalized
    else:
        data_to_write = raw_data if any(isinstance(value, dict) for value in raw_data.values()) else {}
        data_to_write[driver] = normalized

    _ARM_ANGLES_PATH.write_text(
        json.dumps(data_to_write, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return normalized


def _get_defaults(driver: str) -> dict[str, int]:
    if driver not in _DEFAULTS_BY_DRIVER:
        raise ValueError(f"unsupported arm driver: {driver}")
    return _DEFAULTS_BY_DRIVER[driver]


def _read_arm_angles_file() -> dict[str, object]:
    if not _ARM_ANGLES_PATH.exists():
        return {}
    try:
        data = json.loads(_ARM_ANGLES_PATH.read_text(encoding="utf-8"))
    except Exception:
        return {}
    return data if isinstance(data, dict) else {}


def _normalize_angles(data: object, defaults: dict[str, int]) -> dict[str, int]:
    if not isinstance(data, dict):
        data = {}
    normalized: dict[str, int] = {}
    for key, default_value in defaults.items():
        value = data.get(key, default_value)
        try:
            normalized[key] = int(value)
        except (TypeError, ValueError):
            normalized[key] = default_value
    return normalized
