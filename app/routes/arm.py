import json
import os
from flask import Blueprint, request, jsonify

from app.config import config
from app.services import get_control_service
from src.arm_control.angle_config import load_arm_angles, save_arm_angles

arm_bp = Blueprint("arm", __name__, url_prefix="/api/arm")
_DEFAULT_FILE = os.path.join(os.path.dirname(__file__), "..", "..", "arm_angles_default.json")


@arm_bp.route("/angles", methods=["GET", "POST"])
def arm_angles():
    driver = config.arm_driver

    if request.method == "GET":
        return jsonify({
            "driver": driver,
            "angles": load_arm_angles(driver),
        })

    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body is required"}), 400

    body_driver = payload.get("driver", driver)
    if body_driver != driver:
        return jsonify({"error": f"driver mismatch: expected {driver}, got {body_driver}"}), 400

    angles_payload = payload.get("angles", payload)

    try:
        angles = save_arm_angles(driver, angles_payload)
        get_control_service().update_arm_angles(driver, angles)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400

    return jsonify({
        "status": "success",
        "driver": driver,
        "angles": angles,
    })


@arm_bp.route("/angles/default", methods=["GET", "POST"])
def arm_angles_default():
    driver = config.arm_driver
    if request.method == "GET":
        try:
            with open(_DEFAULT_FILE) as f:
                data = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            return jsonify({"error": "default config not found"}), 404
        return jsonify({"driver": driver, "angles": data})

    # POST: 保存默认配置
    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body required"}), 400
    angles_payload = payload.get("angles", payload)
    try:
        with open(_DEFAULT_FILE, "w") as f:
            json.dump(angles_payload, f)
    except IOError as e:
        return jsonify({"error": str(e)}), 500
    return jsonify({"status": "success", "driver": driver, "angles": angles_payload})


@arm_bp.route("/angles/preview", methods=["POST"])
def arm_angles_preview():
    driver = config.arm_driver
    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body is required"}), 400

    body_driver = payload.get("driver", driver)
    if body_driver != driver:
        return jsonify({"error": f"driver mismatch: expected {driver}, got {body_driver}"}), 400

    key = payload.get("key")
    value = payload.get("value")
    angles_payload = payload.get("angles")

    if not isinstance(key, str):
        return jsonify({"error": "key is required"}), 400
    if value is None:
        return jsonify({"error": "value is required"}), 400
    if not isinstance(angles_payload, dict):
        return jsonify({"error": "angles is required"}), 400

    try:
        angle_value = int(value)
        angles = save_arm_angles(driver, angles_payload)
        service = get_control_service()
        service.update_arm_angles(driver, angles)
        service.preview_arm_angle(driver, key, angle_value)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400

    return jsonify({
        "status": "success",
        "driver": driver,
        "key": key,
        "value": angle_value,
        "angles": angles,
    })