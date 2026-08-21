import base64
import os
import re
import subprocess
import sys
import time
from pathlib import Path

from flask import Blueprint, request, jsonify

# WiFi 配置
WIFI_INTERFACE = os.getenv("WIFI_INTERFACE", "wlan1")
WIFI_CTRL_PATH = "/var/run/wpa_supplicant"

# wpa_supplicant 配置文件路径（优先从环境变量获取，否则相对于项目根目录）
_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
WPA_CONFIG_PATH = os.getenv("WPA_SUPPLICANT_CONF",
                            str(_PROJECT_ROOT / "services" / "wpa_supplicant.conf"))

# ── sudo 适配：非 root 用户自动加 sudo 前缀 ──────────────────────────────────
try:
    _IS_ROOT = (os.geteuid() == 0)
except AttributeError:
    _IS_ROOT = True
SUDO = "" if _IS_ROOT else "sudo "
SUDO_LIST = [] if _IS_ROOT else ["sudo"]

wifi_bp = Blueprint("wifi", __name__)


def _socket_exists(path):
    """检查 socket 是否存在（非 root 用户使用 sudo 绕过目录权限）"""
    if _IS_ROOT:
        return os.path.exists(path)
    result = subprocess.run(SUDO_LIST + ["test", "-e", path],
                            capture_output=True, timeout=5)
    return result.returncode == 0


def _wpa_healthy():
    """检查 wpa_supplicant 是否健康响应"""
    try:
        r = subprocess.getoutput(
            f"{SUDO}wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} status")
        return "wpa_state=" in r
    except Exception:
        return False


def _start_wpa():
    """启动 wpa_supplicant（使用配置文件）"""
    # 确保控制接口目录存在
    subprocess.run(SUDO_LIST + ["mkdir", "-p", WIFI_CTRL_PATH],
                   capture_output=True, timeout=5)
    if not _IS_ROOT and not os.path.exists(WIFI_CTRL_PATH):
        return False

    os.system(f"{SUDO}ip link set {WIFI_INTERFACE} up")
    cmd = f"{SUDO}wpa_supplicant -D nl80211 -i {WIFI_INTERFACE} -c {WPA_CONFIG_PATH} -B"
    os.system(cmd)

    # 等待 socket 就绪
    socket_file = f"{WIFI_CTRL_PATH}/{WIFI_INTERFACE}"
    for _ in range(10):
        if _socket_exists(socket_file):
            return True
        time.sleep(0.5)
    return False


def _restart_wpa():
    """杀掉并重启 wpa_supplicant"""
    os.system(f"{SUDO}killall -9 wpa_supplicant 2>/dev/null")
    time.sleep(0.5)
    os.system(f"{SUDO}rm -rf {WIFI_CTRL_PATH}/{WIFI_INTERFACE}")
    time.sleep(0.3)
    return _start_wpa()


def ensure_wpa_env():
    """确保 wpa_supplicant 已初始化且健康运行"""
    # 开发环境（模拟器）下跳过
    if os.name == "nt" or sys.platform == "darwin":
        return False

    # 1. 健康检查：wpa_supplicant 正常响应则直接返回
    if _wpa_healthy():
        return True

    # 2. Socket 存在但 wpa_cli 无响应 → wpa_supplicant 僵死，需重启
    socket_file = f"{WIFI_CTRL_PATH}/{WIFI_INTERFACE}"
    if _socket_exists(socket_file):
        return _restart_wpa()

    # 3. Socket 不存在 → 首次启动
    return _start_wpa()


def get_current_wifi_ip():
    """获取 wlan1 的当前 IP"""
    ip = subprocess.getoutput(
        f"ip addr show {WIFI_INTERFACE} | grep 'inet ' | awk '{{print $2}}' | cut -d/ -f1")
    return ip if ip else "未分配"


def _decode_ssid(ssid: str) -> str:
    """wpa_cli 对非 ASCII 的 SSID 返回 hex 转义序列，如 \\xe4\\xbb\\x95 → 仕。

    逐段还原：每个 \\xHH 还原为对应字节，其余 ASCII 字符原样保留，
    拼成原始字节流后再整体按 UTF-8 解码。这样中英混排（如 家Home）也能正确显示。
    """
    if "\\x" not in ssid:
        return ssid
    try:
        raw = re.sub(
            r"\\x([0-9a-fA-F]{2})",
            lambda m: chr(int(m.group(1), 16)),
            ssid,
        )
        return raw.encode("latin-1").decode("utf-8")
    except Exception:
        return ssid


def get_wifi_list():
    if not ensure_wpa_env():
        return {"list": [], "error": "WPA_INIT_FAILED"}

    os.system(f"{SUDO}wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} scan > /dev/null 2>&1")
    time.sleep(1.5)
    raw_results = subprocess.getoutput(
        f"{SUDO}wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} scan_results")

    current_status = subprocess.getoutput(
        f"{SUDO}wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} status")
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
        "list": sorted(unique_wifi.values(),
                       key=lambda x: (not x['is_connected'], -x['signal'])),
        "connected": connected_ssid
    }


def _is_ascii(text):
    """检查字符串是否仅包含 ASCII 字符"""
    try:
        text.encode("ascii")
        return True
    except (UnicodeEncodeError, UnicodeDecodeError):
        return False


def _wpa_cli_cmd(args, timeout=5):
    """执行 wpa_cli 命令，返回 (returncode, stdout, stderr)"""
    cmd = SUDO_LIST + ["wpa_cli", "-p", WIFI_CTRL_PATH, "-i", WIFI_INTERFACE] + args
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout.strip(), r.stderr.strip()


def do_connect(ssid, password):
    if not ensure_wpa_env():
        return (False, "wpa_supplicant 未就绪")

    # ── SSID 编码：ASCII 用引号包裹，非 ASCII 用 hex 无引号 ──
    if _is_ascii(ssid):
        ssid_arg = f'"{ssid}"'
    else:
        ssid_arg = ssid.encode("utf-8").hex()

    # ── 清除已有网络并断开连接 ──
    rc, out, err = _wpa_cli_cmd(["remove_network", "all"])
    if rc != 0:
        log_msg = f"remove_network 失败: {err or out}"
        print(f"[wifi] {log_msg}")
        return (False, log_msg)
    time.sleep(0.5)

    # ── 添加新网络 ──
    rc, out, err = _wpa_cli_cmd(["add_network"])
    if rc != 0:
        log_msg = f"add_network 失败: {err or out}"
        print(f"[wifi] {log_msg}")
        return (False, log_msg)
    net_id = out.strip().split('\n')[0]
    if not net_id.isdigit():
        return (False, f"add_network 返回异常: {out}")

    # ── 设置 SSID ──
    rc, out, err = _wpa_cli_cmd(["set_network", net_id, "ssid", ssid_arg])
    if rc != 0:
        log_msg = f"set_network ssid 失败: {err or out}"
        print(f"[wifi] {log_msg}")
        return (False, log_msg)

    # ── 设置认证方式 ──
    if password:
        rc, out, err = _wpa_cli_cmd(["set_network", net_id, "psk", f'"{password}"'])
        if rc != 0:
            log_msg = f"set_network psk 失败: {err or out}"
            print(f"[wifi] {log_msg}")
            return (False, log_msg)
    else:
        rc, out, err = _wpa_cli_cmd(["set_network", net_id, "key_mgmt", "NONE"])
        if rc != 0:
            log_msg = f"set_network key_mgmt 失败: {err or out}"
            print(f"[wifi] {log_msg}")
            return (False, log_msg)

    # ── 选择网络并强制重连 ──
    rc, out, err = _wpa_cli_cmd(["select_network", net_id])
    if rc != 0:
        log_msg = f"select_network 失败: {err or out}"
        print(f"[wifi] {log_msg}")
        return (False, log_msg)

    # 强制重连（确保 wpa_supplicant 立即开始连接流程）
    _wpa_cli_cmd(["reassociate"], timeout=3)

    # ── 等待连接完成 ──
    for _ in range(20):
        rc, out, err = _wpa_cli_cmd(["status"])
        if "wpa_state=COMPLETED" in out:
            # 连接成功：先释放旧 DHCP 租约，再获取新 IP
            # 不释放旧租约会导致接口上残留上一个 WiFi 的 IP
            os.system(f"{SUDO}dhclient -r {WIFI_INTERFACE} 2>/dev/null")
            time.sleep(0.5)
            os.system(f"{SUDO}dhclient {WIFI_INTERFACE}")
            ip = subprocess.getoutput(
                f"ip addr show {WIFI_INTERFACE} | grep 'inet ' | awk '{{print $2}}' | cut -d/ -f1")
            return (True, ip.strip() or "获取中...")
        time.sleep(1)

    # 超时，查看当前状态
    rc, out, err = _wpa_cli_cmd(["status"])
    print(f"[wifi] 连接超时, status: {out[:200]}")
    return (False, "连接超时")

# ========== WiFi 路由 ==========

@wifi_bp.route("/ip", methods=["GET"])
def get_ip():
    """获取当前IP（STA模式IP，未连接时返回AP模式IP）"""
    status_raw = subprocess.getoutput(
        f"{SUDO}wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} status")
    ssid_match = re.search(r"^ssid=(.*)$", status_raw, re.MULTILINE)
    current_ssid = _decode_ssid(ssid_match.group(1)) if ssid_match else None

    # 未连接时返回AP模式IP 192.168.4.1
    ip = get_current_wifi_ip() if current_ssid else "192.168.4.1"

    return jsonify({"ip": ip})


@wifi_bp.route("/status", methods=["GET"])
def wifi_status():
    """获取 WiFi 连接状态"""
    status_raw = subprocess.getoutput(
        f"{SUDO}wpa_cli -p {WIFI_CTRL_PATH} -i {WIFI_INTERFACE} status")
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
