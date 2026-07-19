import json
import os
from flask import Blueprint, request, jsonify

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
