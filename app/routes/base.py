from flask import Blueprint, request, jsonify

from app.services import get_control_service
from src.base_control.pwm_channel_config import save_pwm_channels

base_bp = Blueprint("base", __name__, url_prefix="/api/base")


@base_bp.route("/reinitialize", methods=["POST"])
def base_reinitialize():
    return jsonify(get_control_service().reinitialize_motor_pair())

@base_bp.route("/pwm_channels", methods=["GET", "POST"])
def base_pwm_channels():
    if request.method == "GET":
        return jsonify({
            "pwm_channels": get_control_service().get_pwm_channels(),
        })

    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body is required"}), 400

    pwm_channels_payload = payload.get("pwm_channels", payload)
    pwm_channels = save_pwm_channels(payload=pwm_channels_payload)
    get_control_service().update_pwm_channels(pwm_channels)

    return jsonify({
        "status": "success",
        "pwm_channels": pwm_channels,
    })