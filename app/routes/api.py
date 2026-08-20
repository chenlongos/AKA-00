from flask import Blueprint, request, jsonify

from app.services import get_control_service
from app.services.status_reporter import log_command

api_bp = Blueprint("api", __name__, url_prefix="/api")

# 转向角度换算由 ESP32 固件完成（轮径 62mm、轴距 160mm、PPR 4680），
# Python 侧只传角度（°），不做弧长/轴距换算。


@api_bp.route('/control', methods=['GET'])
def action_control():
    """动作控制 API（兼容实验室 Blockly/Python 外部调用格式）

    Query params:
      action   - up|down|left|right|stop|grab|release
      speed    - 电机百分比 (1~100)，默认 50
      distance - 移动距离 毫米 (mm)，up/down 有效
      angle    - 转动角度 度 (°)，left/right 有效
      time     - 持续时间 毫秒（优先级低于 distance/angle）
    """
    action = request.args.get('action')
    speed_pct = float(request.args.get('speed', 50))
    speed_pct = max(1, min(100, speed_pct))  # clamp

    motor_speed = int(round(speed_pct))

    distance_mm = request.args.get('distance', type=float)
    angle_deg   = request.args.get('angle', type=float)

    # 参数校验：distance 仅对 up/down 有效，angle 仅对 left/right 有效
    if angle_deg is not None and action not in ("left", "right"):
        return jsonify({"status": "error",
                        "message": "angle 仅对 left/right 动作有效"}), 400
    if distance_mm is not None and action not in ("up", "down"):
        return jsonify({"status": "error",
                        "message": "distance 仅对 up/down 动作有效"}), 400

    try:
        if action in ("up", "down"):
            if distance_mm:
                direction = {"up": "forward", "down": "backward"}[action]
                return jsonify(get_control_service().move_distance(
                    direction, distance_mm, motor_speed))
        elif action in ("left", "right"):
            if angle_deg:
                direction = {"left": "left", "right": "right"}[action]
                return jsonify(get_control_service().move_distance(
                    direction, angle_deg, motor_speed))

        # 无 distance/angle → 时间控制（电机仍走 PID 闭环，由 motor-bridge sleep T ms 后停止）
        milliseconds = int(float(request.args.get('time', 0)))
        return jsonify(get_control_service().execute_action(
            action, motor_speed, milliseconds))
    except ValueError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400