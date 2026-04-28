from __future__ import annotations

import json
from pathlib import Path

_PWM_CHANNEL_CONFIG_PATH = Path(__file__).resolve().parents[2] / "pwm_channels.json"

_DEFAULT_PWM_CHANNELS = {
    "left_ch1": 0,
    "left_ch2": 1,
    "right_ch1": 2,
    "right_ch2": 3,
}


def load_pwm_channels(_config=None) -> dict[str, int]:
    """加载 PWM 通道配置。"""
    if not _PWM_CHANNEL_CONFIG_PATH.exists():
        return _DEFAULT_PWM_CHANNELS.copy()

    try:
        data = json.loads(_PWM_CHANNEL_CONFIG_PATH.read_text(encoding="utf-8"))
    except Exception:
        return _DEFAULT_PWM_CHANNELS.copy()

    if not isinstance(data, dict):
        return _DEFAULT_PWM_CHANNELS.copy()

    normalized: dict[str, int] = {}
    for key, default_value in _DEFAULT_PWM_CHANNELS.items():
        try:
            normalized[key] = int(data.get(key, default_value))
        except (TypeError, ValueError):
            normalized[key] = default_value
    return normalized


def save_pwm_channels(_config=None, payload: dict[str, object] = None) -> dict[str, int]:
    """保存 PWM 通道配置。"""
    payload = payload or {}
    normalized: dict[str, int] = {}
    for key, default_value in _DEFAULT_PWM_CHANNELS.items():
        try:
            normalized[key] = int(payload.get(key, default_value))
        except (TypeError, ValueError):
            normalized[key] = default_value

    _PWM_CHANNEL_CONFIG_PATH.write_text(
        json.dumps(normalized, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return normalized
