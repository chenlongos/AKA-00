from __future__ import annotations

import json
from pathlib import Path

_PWM_CHANNEL_CONFIG_PATH = Path(__file__).resolve().parents[2] / "pwm_channels.json"


def _defaults(config) -> dict[str, int]:
    return {
        "left_ch1": config.base_left_ch1,
        "left_ch2": config.base_left_ch2,
        "right_ch1": config.base_right_ch1,
        "right_ch2": config.base_right_ch2,
    }


def load_pwm_channels(config) -> dict[str, int]:
    defaults = _defaults(config)
    if not _PWM_CHANNEL_CONFIG_PATH.exists():
        return defaults

    try:
        data = json.loads(_PWM_CHANNEL_CONFIG_PATH.read_text(encoding="utf-8"))
    except Exception:
        return defaults

    if not isinstance(data, dict):
        return defaults

    normalized: dict[str, int] = {}
    for key, default_value in defaults.items():
        try:
            normalized[key] = int(data.get(key, default_value))
        except (TypeError, ValueError):
            normalized[key] = default_value
    return normalized


def save_pwm_channels(config, payload: dict[str, object]) -> dict[str, int]:
    defaults = _defaults(config)
    normalized: dict[str, int] = {}
    for key, default_value in defaults.items():
        try:
            normalized[key] = int(payload.get(key, default_value))
        except (AttributeError, TypeError, ValueError):
            normalized[key] = default_value

    _PWM_CHANNEL_CONFIG_PATH.write_text(
        json.dumps(normalized, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return normalized
