import json
import os
from flask import Blueprint, request, jsonify

from app import motor_config
from app.services import get_control_service

config_bp = Blueprint("config", __name__, url_prefix="/api/config")

CONFIG_FILE = os.path.join(os.path.dirname(__file__), "..", "..", "speed_config.json")
DEFAULTS = {"forward_speed": 50, "turn_speed": 50}


def load_speed_config():
    try:
        with open(CONFIG_FILE) as f:
            data = json.load(f)
        return {
            "forward_speed": int(data.get("forward_speed", DEFAULTS["forward_speed"])),
            "turn_speed": int(data.get("turn_speed", DEFAULTS["turn_speed"])),
        }
    except (FileNotFoundError, json.JSONDecodeError, ValueError):
        return dict(DEFAULTS)


def save_speed_config(data):
    cfg = {
        "forward_speed": int(data.get("forward_speed", DEFAULTS["forward_speed"])),
        "turn_speed": int(data.get("turn_speed", DEFAULTS["turn_speed"])),
    }
    with open(CONFIG_FILE, "w") as f:
        json.dump(cfg, f)
    return cfg


@config_bp.route("/speed", methods=["GET", "POST"])
def speed_config():
    if request.method == "GET":
        return jsonify(load_speed_config())

    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body required"}), 400

    cfg = save_speed_config(payload)
    return jsonify(cfg)


# ── 左右方向切换（底盘电机接线标定）──
#
# 现场问题：部分单体底盘电机接线左右对调，表现为“前进后退正常、只有左右转反”。
# 这里把代理层的 swap_lr 开关暴露给网页：改了立即生效（下发/回读/闭环转向一起翻），
# 并落 motor_config.json —— 与 C++ 路径共用同一份（见 app/motor_config.py）。

@config_bp.route("/motor", methods=["GET", "POST"])
def motor_calib():
    if request.method == "GET":
        return jsonify({"ok": True, **motor_config.load()})

    payload = request.get_json(silent=True)
    swap_lr = payload.get("swap_lr") if isinstance(payload, dict) else None
    if not isinstance(swap_lr, bool):
        return jsonify({"error": "swap_lr (true/false) required"}), 400

    # 先落盘再改内存：写失败就整体失败，不留“改了但不持久”的状态
    try:
        cfg = motor_config.save(swap_lr)
    except OSError as e:
        return jsonify({"error": f"write motor_config.json failed: {e}"}), 500
    get_control_service().set_swap_lr(cfg["swap_lr"])
    return jsonify({"ok": True, **cfg})
