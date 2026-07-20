import threading

from flask import Blueprint, request, jsonify

from app.services import get_control_service

api_bp = Blueprint("api", __name__, url_prefix="/api")

# 物理常数
MAX_SPEED_MPS = 0.5      # 100% 电机速度 ≈ 0.5 m/s
WHEEL_BASE_M = 0.15      # 左右轮间距 (m)，用于转角计算
PI = 3.1415926535


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

    try:
        if action in ("up", "down", "left", "right"):
            if distance_cm:
                direction = {"up": "forward", "down": "backward",
                             "left": "left", "right": "right"}[action]
                # 后台线程执行闭环控制，避免阻塞 Tornado
                service = get_control_service()
                threading.Thread(
                    target=service.move_distance,
                    args=(direction, distance_cm * 10, motor_speed),
                    daemon=True
                ).start()
                return jsonify({"status": "started", "action": action,
                                "mode": "closed_loop", "distance_mm": distance_cm * 10})
            elif angle_deg:
                arc_mm = (angle_deg / 360.0) * PI * WHEEL_BASE_M * 1000.0
                direction = {"left": "left", "right": "right"}[action]
                service = get_control_service()
                threading.Thread(
                    target=service.move_distance,
                    args=(direction, arc_mm, motor_speed),
                    daemon=True
                ).start()
                return jsonify({"status": "started", "action": action,
                                "mode": "closed_loop", "angle_deg": angle_deg})

        # 无 distance/angle → 开环时间控制
        milliseconds = int(float(request.args.get('time', 0)))
        return jsonify(get_control_service().execute_action(
            action, motor_speed, milliseconds))
    except ValueError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400
