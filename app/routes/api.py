from flask import Blueprint, request, jsonify

from app.services import get_control_service
from app.services.status_reporter import log_command

api_bp = Blueprint("api", __name__, url_prefix="/api")

# 物理常数
MAX_SPEED_MPS = 0.5      # 100% 电机速度 ≈ 0.5 m/s
# 转向角度换算由 ESP32 固件完成（轮径 62mm、轴距 160mm、PPR 4680），
# Python 侧只传角度（°），不做弧长/轴距换算。


def _mps_to_motor(mps: float) -> int:
    """线速度 m/s → 电机 -100..100"""
    return max(-100, min(100, round(mps / MAX_SPEED_MPS * 100)))


@api_bp.route('/control', methods=['GET'])
def action_control():
    """动作控制 API（兼容实验室 Blockly/Python 外部调用格式）

    Query params:
      action   - up|down|left|right|stop|grab|release
      speed    - 线速度 m/s (0.01~0.5)，默认 0.25
      distance - 移动距离 厘米 (cm)，up/down 有效
      angle    - 转动角度 度 (°)，left/right 有效
      time     - 持续时间 毫秒（优先级低于 distance/angle）
    """
    action = request.args.get('action')
    speed_mps = float(request.args.get('speed', 0.25))
    speed_mps = max(0.01, min(0.5, speed_mps))  # clamp

    motor_speed = _mps_to_motor(speed_mps)

    distance_cm = request.args.get('distance', type=float)
    angle_deg   = request.args.get('angle', type=float)

    # 参数校验：distance 仅对 up/down 有效，angle 仅对 left/right 有效
    if angle_deg is not None and action not in ("left", "right"):
        return jsonify({"status": "error",
                        "message": "angle 仅对 left/right 动作有效"}), 400
    if distance_cm is not None and action not in ("up", "down"):
        return jsonify({"status": "error",
                        "message": "distance 仅对 up/down 动作有效"}), 400

    try:
        if action in ("up", "down"):
            if distance_cm:
                direction = {"up": "forward", "down": "backward"}[action]
                return jsonify(get_control_service().move_distance(
                    direction, distance_cm * 10, motor_speed))
        elif action in ("left", "right"):
            if angle_deg:
                direction = {"left": "left", "right": "right"}[action]
                return jsonify(get_control_service().move_distance(
                    direction, angle_deg, motor_speed))

        # 无 distance/angle → 开环时间控制
        milliseconds = int(float(request.args.get('time', 0)))
        return jsonify(get_control_service().execute_action(
            action, motor_speed, milliseconds))
    except ValueError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400
