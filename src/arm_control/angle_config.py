"""舵机角度配置 —— 加载/保存/迁移 arm_angles.json。

配置结构（语义化）:
  {
    "grab_position": {"servo0": 245, "servo1": 180, "servo2": 150},
    "lift_position": {"servo0": 200, "servo1": 180, "servo2": 90},
    "gripper_open": 150,
    "gripper_close": 90
  }
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

ZP10S_DRIVER = "zp10s"
STS3215_DRIVER = "sts3215"

# ---- 默认值（新格式） ----

DEFAULT_ZP10S_ARM_ANGLES: dict[str, Any] = {
    "grab_position": {"servo0": 245, "servo1": 180},
    "lift_position": {"servo0": 200, "servo1": 180},
    "gripper_open": 150,
    "gripper_close": 90,
}

DEFAULT_STS3215_ARM_ANGLES: dict[str, Any] = {
    "grab_position": {"servo1": 1850, "servo2": 2650},
    "lift_position": {"servo1": 2300, "servo2": 2100},
    "gripper_open": 4000,
    "gripper_close": 3000,
}

_DEFAULTS_BY_DRIVER: dict[str, dict[str, Any]] = {
    ZP10S_DRIVER: DEFAULT_ZP10S_ARM_ANGLES,
    STS3215_DRIVER: DEFAULT_STS3215_ARM_ANGLES,
}

_ARM_ANGLES_PATH = Path(__file__).resolve().parents[2] / "arm_angles.json"

# ---- 公共 API ----


def get_arm_angles_path() -> Path:
    return _ARM_ANGLES_PATH


def load_arm_angles(driver: str) -> dict[str, Any]:
    """加载角度配置，自动迁移旧格式。"""
    defaults = _get_defaults(driver)
    raw_data = _read_arm_angles_file()

    # 提取对应 driver 的数据
    if isinstance(raw_data.get(driver), dict):
        source = raw_data[driver]
    elif driver == ZP10S_DRIVER:
        source = raw_data
    else:
        source = {}

    # 检测并迁移旧格式
    if _is_old_format(source):
        source = _migrate_old_format(source, driver)
        # 写回迁移后的格式
        _write_migrated(driver, source, raw_data)

    return _normalize_angles(source, defaults)


def save_arm_angles(driver: str, angles: dict[str, object]) -> dict[str, Any]:
    """保存角度配置（自动标准化）。"""
    defaults = _get_defaults(driver)
    normalized = _normalize_angles(angles, defaults)
    raw_data = _read_arm_angles_file()

    # 检测是否为多驱动格式：顶层 key 是否包含已知驱动名
    is_multi_driver = any(k in _DEFAULTS_BY_DRIVER for k in raw_data if isinstance(k, str))
    if driver == ZP10S_DRIVER and not is_multi_driver:
        data_to_write: dict[str, object] = normalized
    else:
        data_to_write = raw_data if is_multi_driver else {}
        data_to_write[driver] = normalized

    _ARM_ANGLES_PATH.write_text(
        json.dumps(data_to_write, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return normalized


# ---- 便捷访问函数 ----


def get_grab_position(angles: dict[str, Any], servo_key: str, driver: str = ZP10S_DRIVER) -> int:
    """读取夹取位置中某个舵机的角度。"""
    gp = angles.get("grab_position", {})
    if isinstance(gp, dict):
        return int(gp.get(servo_key, _default_grab_servo(driver, servo_key)))
    return _default_grab_servo(driver, servo_key)


def get_lift_position(angles: dict[str, Any], servo_key: str, driver: str = ZP10S_DRIVER) -> int:
    """读取抬起位置中某个舵机的角度。"""
    lp = angles.get("lift_position", {})
    if isinstance(lp, dict):
        return int(lp.get(servo_key, _default_lift_servo(driver, servo_key)))
    return _default_lift_servo(driver, servo_key)


def get_gripper_open(angles: dict[str, Any], driver: str = ZP10S_DRIVER) -> int:
    """读取夹爪张开角度。"""
    defaults = _get_defaults(driver)
    val = angles.get("gripper_open", defaults["gripper_open"])
    try:
        return int(val)
    except (TypeError, ValueError):
        return int(defaults["gripper_open"])


def get_gripper_close(angles: dict[str, Any], driver: str = ZP10S_DRIVER) -> int:
    """读取夹爪闭合角度。"""
    defaults = _get_defaults(driver)
    val = angles.get("gripper_close", defaults["gripper_close"])
    try:
        return int(val)
    except (TypeError, ValueError):
        return int(defaults["gripper_close"])


def _default_grab_servo(driver: str, servo_key: str) -> int:
    defaults = _get_defaults(driver)
    gp = defaults.get("grab_position", {})
    return int(gp.get(servo_key, 0))


def _default_lift_servo(driver: str, servo_key: str) -> int:
    defaults = _get_defaults(driver)
    lp = defaults.get("lift_position", {})
    return int(lp.get(servo_key, 0))


# ---- 内部函数 ----


def _get_defaults(driver: str) -> dict[str, Any]:
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


def _normalize_angles(data: object, defaults: dict[str, Any]) -> dict[str, Any]:
    """标准化角度配置，用默认值填充缺失字段。"""
    if not isinstance(data, dict):
        data = {}

    result: dict[str, Any] = {}

    # gripper_open / gripper_close: 标量
    for scalar_key in ("gripper_open", "gripper_close"):
        val = data.get(scalar_key, defaults[scalar_key])
        try:
            result[scalar_key] = int(val)
        except (TypeError, ValueError):
            result[scalar_key] = int(defaults[scalar_key])

    # grab_position / lift_position: 嵌套 dict
    for group_key in ("grab_position", "lift_position"):
        default_group = defaults.get(group_key, {})
        src_group = data.get(group_key, {})
        if not isinstance(src_group, dict):
            src_group = {}
        normalized_group: dict[str, int] = {}
        for servo_key, default_val in default_group.items():
            val = src_group.get(servo_key, default_val)
            try:
                normalized_group[servo_key] = int(val)
            except (TypeError, ValueError):
                normalized_group[servo_key] = int(default_val)
        result[group_key] = normalized_group

    return result


def _is_old_format(data: object) -> bool:
    """检测是否为旧格式（存在 servoX_... 键）。"""
    if not isinstance(data, dict):
        return False
    return any(k.startswith("servo") and ("_prepare" in k or "_lift" in k or "_grab" in k or "_approach" in k or "_enter" in k)
               for k in data if isinstance(k, str))


def _migrate_old_format(data: dict[str, object], driver: str) -> dict[str, Any]:
    """将旧格式角度配置迁移到新语义化格式。"""
    if driver == ZP10S_DRIVER:
        return {
            "grab_position": {
                "servo0": int(data.get("servo0_prepare", 245)),
                "servo1": int(data.get("servo1_prepare", 180)),
            },
            "lift_position": {
                "servo0": int(data.get("servo0_lift", 200)),
                "servo1": int(data.get("servo1_lift", 180)),
            },
            "gripper_open": int(data.get("servo2_prepare", 150)),
            "gripper_close": int(data.get("servo2_grab", 90)),
        }
    elif driver == STS3215_DRIVER:
        return {
            "grab_position": {
                "servo1": int(data.get("servo1_enter", 1850)),
                "servo2": int(data.get("servo2_enter", 2650)),
            },
            "lift_position": {
                "servo1": int(data.get("servo1_lift", 2300)),
                "servo2": int(data.get("servo2_lift", 2100)),
            },
            "gripper_open": int(data.get("servo3_prepare", 4000)),
            "gripper_close": int(data.get("servo3_grab", 3000)),
        }
    raise ValueError(f"unsupported arm driver: {driver}")


def _write_migrated(driver: str, migrated: dict[str, Any], raw_data: dict[str, object]) -> None:
    """将迁移后的配置写回文件。"""
    is_multi_driver = any(k in _DEFAULTS_BY_DRIVER for k in raw_data if isinstance(k, str))
    if driver == ZP10S_DRIVER and not is_multi_driver:
        data_to_write: dict[str, object] = migrated
    else:
        data_to_write = raw_data if is_multi_driver else {}
        data_to_write[driver] = migrated
    _ARM_ANGLES_PATH.write_text(
        json.dumps(data_to_write, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
