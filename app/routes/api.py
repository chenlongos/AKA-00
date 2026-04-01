import socket
import struct

try:
    import fcntl
    _HAS_FCNTL = True
except Exception:
    _HAS_FCNTL = False

from flask import Blueprint, request, jsonify

from app.config import load_hardware_config
from app.services import get_control_service

api_bp = Blueprint("api", __name__)


@api_bp.route("/ip")
def ip():
    return jsonify({
        "ip": get_ip(load_hardware_config().wifi_interface)
    })


@api_bp.route("/motor_status")
def motor_status():
    """获取小车左右轮当前状态。"""
    timestamp = request.args.get("timestamp")
    if timestamp is None:
        return jsonify({"error": "timestamp is required"}), 400
    timestamp = int(timestamp)
    return jsonify(get_control_service().get_motor_status(timestamp))


@api_bp.route('/control', methods=['GET'])
def control():
    action = request.args.get('action')
    speed = int(request.args.get('speed', 50))
    milliseconds = float(request.args.get('time', 0))

    try:
        return jsonify(get_control_service().execute_action(action, speed, milliseconds))
    except ValueError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400


def get_ip(ifname="wlan0"):
    if not _HAS_FCNTL:
        return socket.gethostbyname(socket.gethostname())
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    return socket.inet_ntoa(
        fcntl.ioctl(
            s.fileno(),
            0x8915,
            struct.pack('256s', ifname[:15].encode('utf-8'))
        )[20:24]
    )

@api_bp.route('/raw_command', methods=['GET'])
def raw_command():
    cmd = request.args.get('cmd', '')
    return jsonify(get_control_service().send_raw_command(cmd))


@api_bp.route('/motor_direct', methods=['GET'])
def motor_direct():
    """直接设置左右轮速度

    Query params:
        left: int  左轮速度 (-255 ~ 255)
        right: int 右轮速度 (-255 ~ 255)
    """
    left = int(request.args.get('left', 0))
    right = int(request.args.get('right', 0))

    return jsonify(get_control_service().set_motor_speed(left, right))


@api_bp.route("/heartbeat")
def heartbeat():
    """心跳检测API，用于检查服务是否存活。"""
    config = load_hardware_config()
    mac_address = get_mac_address(config.wifi_interface)
    return jsonify({
        "status": "ok",
        "service": "AKA-00",
        "mac_address": mac_address
    })


def get_mac_address(ifname="wlan0"):
    """获取指定网络接口的MAC地址。"""
    try:
        with open(f"/sys/class/net/{ifname}/address", "r") as f:
            return f.read().strip()
    except Exception:
        return "unknown"
