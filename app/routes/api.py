from flask import Blueprint, request, jsonify

from app.services import get_control_service

api_bp = Blueprint("api", __name__, url_prefix="/api")


# ===========================
# 动作控制（统一入口）
# ===========================

@api_bp.route('/control', methods=['GET'])
def action_control():
    """动作控制API，支持 up/down/left/right/stop/grab/release 等"""
    action = request.args.get('action')
    speed = int(request.args.get('speed', 50))
    milliseconds = float(request.args.get('time', 0))

    try:
        return jsonify(get_control_service().execute_action(action, speed, milliseconds))
    except ValueError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400


# ===========================
# 注册子模块
# ===========================

from app.routes.system import system_bp
from app.routes.motor import motor_bp
from app.routes.arm import arm_bp
from app.routes.base import base_bp
from app.routes.camera import camera_bp
from app.routes.demo import demo_bp