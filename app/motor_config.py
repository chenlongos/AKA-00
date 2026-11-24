"""底盘左右接线标定（网页「左右方向切换」）的落盘读写。

文件：项目根 motor_config.json        {"swap_lr": false}

为什么单独一个文件：本项目有 Python（run.py）和 C++（aka-capp）两条并行路径，
C++ 侧的标定在 config.toml 的 [motor] swap_lr 里，而 Python 路径根本不读 config.toml
（配置是 app/config.py 里的常量）。网页上这一个开关要两条路径都能用，所以落成两边
都读写的小 JSON，放在同一个应用根目录下 —— C++ 侧见
cpp/capp/src/services/motor_config.cpp，同一个文件名、同一个位置。
"""

import json
import os

_PATH = os.path.join(os.path.dirname(__file__), "..", "motor_config.json")
_DEFAULT = {"swap_lr": False}


def load() -> dict:
    """读标定；文件缺失/损坏时返回默认（不左右对调）。"""
    try:
        with open(_PATH) as f:
            data = json.load(f)
        return {"swap_lr": bool(data.get("swap_lr", False))}
    except (OSError, json.JSONDecodeError, ValueError, TypeError, AttributeError):
        return dict(_DEFAULT)


def save(swap_lr: bool) -> dict:
    """写标定（只写这一个字段）；.tmp + rename，断电不会留下半份文件。"""
    cfg = {"swap_lr": bool(swap_lr)}
    tmp = _PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f)
    os.replace(tmp, _PATH)
    return cfg