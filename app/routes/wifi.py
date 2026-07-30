import base64
import os
import re
import subprocess
import sys
import time

from flask import Blueprint, request, jsonify

# WiFi 配置
WIFI_INTERFACE = os.getenv("WIFI_INTERFACE", "wlan1")
WIFI_CTRL_PATH = "/var/run/wpa_supplicant"

wifi_bp = Blueprint("wifi", __name__)


def ensure_wpa_env():
    """确保 wpa_supplicant 已初始化"""
    # 开发环境（模拟器）下跳过
    if os.name == "nt" or sys.platform == "darwin":
        return False

    if not os.path.exists(WIFI_CTRL_PATH):
        try:
            os.makedirs(WIFI_CTRL_PATH, exist_ok=True)
        except PermissionError:
            return False

    socket_file = f"{WIFI_CTRL_PATH}/{WIFI_INTERFACE}"
    if not os.path.exists(socket_file):
        os.system("killall -9 wpa_supplicant 2>/dev/null")
        time.sleep(0.5)
        os.system(f"rm -rf {socket_file}")
        os.system(f"ip link set {WIFI_INTERFACE} down")
        os.system(f"ip link set {WIFI_INTERFACE} up")
        time.sleep(0.5)
        cmd = f"wpa_supplicant -D nl80211 -i {WIFI_INTERFACE} -C {WIFI_CTRL_PATH} -B"
        os.system(cmd)
        for _ in range(10):
            if os.path.exists(socket_file):
                return True
            time.sleep(0.5)
        return False
    return True


def get_current_wifi_ip():
    """获取 wlan1 的当前 IP"""
    ip = subprocess.getoutput(f"ip addr show {WIFI_INTERFACE} | grep 'inet ' | awk '{{print $2}}' | cut -d/ -f1")
    return ip if ip else "未分配"


def _decode_ssid(ssid: str) -> str:
    """wpa_cli 对非 ASCII 的 SSID 返回 hex 转义序列，如 \\xe4\\xbb\\x95 → 仕"""
    if "\\x" not in ssid:
        return ssid
    try:
        hex_str = ssid.replace("\\x", "")
        return bytes.fromhex(hex_str).decode("utf-8")
    except Exception:
        return ssid


def get_wifi_list():
    if not ensure_wpa_env():
        return {"list": [], "error": "WPA_INIT_FAILED"}

    os.system(f"wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} scan > /dev/null 2>&1")
    time.sleep(1.5)
    raw_results = subprocess.getoutput(f"wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} scan_results")

    current_status = subprocess.getoutput(f"wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} status")
    connected_ssid = None
    if "wpa_state=COMPLETED" in current_status:
        ssid_match = re.search(r"^ssid=(.*)$", current_status, re.MULTILINE)
        if ssid_match:
            connected_ssid = _decode_ssid(ssid_match.group(1))

    unique_wifi = {}
    lines = raw_results.split('\n')
    for line in lines[1:]:
        parts = line.split('\t')
        if len(parts) >= 5:
            ssid = _decode_ssid(parts[4].strip())
            if not ssid:
                continue
            signal = int(parts[2])
            safe_id = base64.b64encode(ssid.encode()).decode().replace('=', '')
            if ssid not in unique_wifi or signal > unique_wifi[ssid]['signal']:
                unique_wifi[ssid] = {
                    "ssid": ssid,
                    "id": safe_id,
                    "signal": signal,
                    "secured": not (parts[3] == "[ESS]" or parts[3] == "[WPS][ESS]"),
                    "is_connected": (ssid == connected_ssid)
                }

    return {
        "list": sorted(unique_wifi.values(), key=lambda x: (not x['is_connected'], -x['signal'])),
        "connected": connected_ssid
    }


def do_connect(ssid, password):
    ensure_wpa_env()
    # 中文 SSID 用 hex 编码传给 wpa_cli，避免 shell 乱码
    ssid_hex = ssid.encode("utf-8").hex()

    subprocess.run(
        ["wpa_cli", "-p", WIFI_CTRL_PATH, "-i", WIFI_INTERFACE, "remove_network", "all"],
        capture_output=True, timeout=5)
    r = subprocess.run(
        ["wpa_cli", "-p", WIFI_CTRL_PATH, "-i", WIFI_INTERFACE, "add_network"],
        capture_output=True, text=True, timeout=5)
    net_id = r.stdout.strip().split('\n')[0]

    subprocess.run(
        ["wpa_cli", "-p", WIFI_CTRL_PATH, "-i", WIFI_INTERFACE,
         "set_network", net_id, "ssid", f'"{ssid_hex}"'],
        capture_output=True, timeout=5)
    if password:
        subprocess.run(
            ["wpa_cli", "-p", WIFI_CTRL_PATH, "-i", WIFI_INTERFACE,
             "set_network", net_id, "psk", f'"{password}"'],
            capture_output=True, timeout=5)
    else:
        subprocess.run(
            ["wpa_cli", "-p", WIFI_CTRL_PATH, "-i", WIFI_INTERFACE,
             "set_network", net_id, "key_mgmt", "NONE"],
            capture_output=True, timeout=5)

    subprocess.run(
        ["wpa_cli", "-p", WIFI_CTRL_PATH, "-i", WIFI_INTERFACE,
         "select_network", net_id],
        capture_output=True, timeout=5)

    for _ in range(15):
        status = subprocess.getoutput(f"wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} status")
        if "wpa_state=COMPLETED" in status:
            os.system(f"udhcpc -i {WIFI_INTERFACE} -n -q -T 5")
            ip = subprocess.getoutput(f"ip addr show {WIFI_INTERFACE} | grep 'inet ' | awk '{{print $2}}' | cut -d/ -f1")
            return True, ip
        time.sleep(1)
    return False, "连接超时"


# ========== WiFi 路由 ==========

@wifi_bp.route("/ip", methods=["GET"])
def get_ip():
    """获取当前IP（STA模式IP，未连接时返回AP模式IP）"""
    status_raw = subprocess.getoutput(f"wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} status")
    ssid_match = re.search(r"^ssid=(.*)$", status_raw, re.MULTILINE)
    current_ssid = _decode_ssid(ssid_match.group(1)) if ssid_match else None

    # 未连接时返回AP模式IP 192.168.4.1
    ip = get_current_wifi_ip() if current_ssid else "192.168.4.1"

    return jsonify({"ip": ip})


@wifi_bp.route("/status", methods=["GET"])
def wifi_status():
    """获取 WiFi 连接状态"""
    status_raw = subprocess.getoutput(f"wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} status")
    ssid_match = re.search(r"^ssid=(.*)$", status_raw, re.MULTILINE)
    current_ssid = _decode_ssid(ssid_match.group(1)) if ssid_match else None

    # 未连接时返回AP模式IP 192.168.4.1
    ip = get_current_wifi_ip() if current_ssid else "192.168.4.1"

    return jsonify({
        "ssid": current_ssid,
        "ip": ip
    })


@wifi_bp.route("/scan", methods=["GET"])
def wifi_scan():
    """扫描 WiFi 列表"""
    return jsonify(get_wifi_list())


@wifi_bp.route("/connect", methods=["POST"])
def wifi_connect():
    """连接 WiFi"""
    data = request.get_json()
    ssid = data.get("ssid", "")
    password = data.get("password", "")
    if not ssid:
        return jsonify({"error": "ssid 不能为空"}), 400
    success, info = do_connect(ssid, password)
    if success:
        return jsonify({"ip": info}), 200
    return jsonify({"error": info}), 408
