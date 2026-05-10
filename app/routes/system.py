from flask import Blueprint, jsonify
from app.routes._utils import get_ip, get_mac_address

system_bp = Blueprint("system", __name__, url_prefix="/api/system")


@system_bp.route("/ip")
def ip():
    return jsonify({"ip": get_ip("wlan0")})


@system_bp.route("/heartbeat")
def heartbeat():
    mac_address = get_mac_address("wlan0")
    return jsonify({
        "status": "ok",
        "service": "AKA-00",
        "mac_address": mac_address
    })