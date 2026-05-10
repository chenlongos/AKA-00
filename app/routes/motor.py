from flask import Blueprint, request, jsonify

from src.state import get_state_collector
from app.services import get_control_service

motor_bp = Blueprint("motor", __name__, url_prefix="/api/motor")


@motor_bp.route("/status")
def motor_status():
    status = get_state_collector().get_status()
    return jsonify({
        "left_speed": status.left_speed,
        "right_speed": status.right_speed,
    })


@motor_bp.route("/direct", methods=["GET"])
def motor_direct():
    left = int(float(request.args.get('left', 0)))
    right = int(float(request.args.get('right', 0)))
    duration = float(request.args.get('duration', 0))

    try:
        result = get_control_service().run_motor(left, right, duration)
        get_state_collector().set_action(left, right)
        status = get_state_collector().get_status()
        result["left_speed"] = status.left_speed
        result["right_speed"] = status.right_speed
        return jsonify(result)
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@motor_bp.route("/raw_command", methods=["GET"])
def raw_command():
    cmd = request.args.get('cmd', '')
    return jsonify(get_control_service().send_raw_command(cmd))