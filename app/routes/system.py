from flask import Blueprint, jsonify
from app.routes._utils import get_wifi_ip, get_mac_address
from app.services.status_reporter import _get_cpu_usage, _get_mem_usage, _get_disk, _get_uptime

system_bp = Blueprint("system", __name__, url_prefix="/api/system")


@system_bp.route("/ip")
def ip():
    return jsonify({"ip": get_wifi_ip()})


@system_bp.route("/heartbeat")
def heartbeat():
    mac_address = get_mac_address("wlan0")
    return jsonify({
        "status": "ok",
        "service": "AKA-00",
        "mac_address": mac_address,
        "cpu": _get_cpu_usage(),
        "mem": _get_mem_usage(),
        "disk": _get_disk(),
        "uptime": _get_uptime(),
    })