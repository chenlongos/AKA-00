"""状态上报服务 — 定期向云端报告设备在线状态和资源使用情况"""
import json
import os
import subprocess
import threading
import time
import urllib.request

from app.config import config
from app.routes._utils import get_mac_address

_REPORT_URL = config.status_report_url or os.environ.get("STATUS_REPORT_URL", "")
_INTERVAL = int(os.environ.get("STATUS_REPORT_INTERVAL", "300"))
_command_log = []  # 最近的控制命令
_MAX_COMMANDS = 20


def _get_cpu_usage():
    """读取 /proc/stat 计算 CPU 使用率"""
    try:
        with open("/proc/stat") as f:
            fields = f.readline().split()
        if len(fields) < 5:
            return 0
        total = sum(int(x) for x in fields[1:])
        idle = int(fields[4])
        return int((1 - idle / total) * 100) if total > 0 else 0
    except Exception:
        return 0


def _get_mem_usage():
    """读取 /proc/meminfo 计算内存使用率"""
    try:
        mem = {}
        with open("/proc/meminfo") as f:
            for line in f:
                parts = line.split(":")
                if len(parts) >= 2:
                    mem[parts[0].strip()] = int(parts[1].strip().split()[0])
        total = mem.get("MemTotal", 1)
        available = mem.get("MemAvailable", mem.get("MemFree", 0))
        return int((1 - available / total) * 100) if total > 0 else 0
    except Exception:
        return 0


def _get_uptime():
    try:
        with open("/proc/uptime") as f:
            return int(float(f.read().split()[0]))
    except Exception:
        return 0

def _get_disk():
    try:
        s = os.statvfs("/")
        return int((1 - s.f_bavail / s.f_blocks) * 100) if s.f_blocks > 0 else 0
    except Exception:
        return 0

def _get_version():
    try:
        vf = os.path.join(os.path.dirname(__file__), "..", "..", "VERSION")
        with open(vf) as f:
            return f.read().strip().split('@')[0]
    except Exception:
        return "unknown"

def _get_robot_status():
    """获取机器人运行状态"""
    try:
        from src.state import get_state_collector
        s = get_state_collector().get_status()
        return {
            "left_speed": round(s.left_speed, 2),
            "right_speed": round(s.right_speed, 2),
            "is_moving": abs(s.left_speed) > 0.01 or abs(s.right_speed) > 0.01,
            "gripper": s.gripper_status,
        }
    except Exception:
        return {}

def _get_camera_status():
    """获取摄像头状态"""
    try:
        from app.services.camera_service import CameraService
        return {"camera_on": CameraService.get_instance().is_available()}
    except Exception:
        return {"camera_on": False}

def log_command(cmd: dict):
    """记录控制命令（供外部调用）"""
    _command_log.append({"ts": int(time.time()), **cmd})
    if len(_command_log) > _MAX_COMMANDS:
        _command_log.pop(0)


def _report(action: str, extra: dict = None):
    if not _REPORT_URL:
        return
    try:
        payload = {
            "action": action,
            "physicalAddress": get_mac_address("wlan0"),
            "payload": {
                "cpu": _get_cpu_usage(),
                "mem": _get_mem_usage(),
                "disk": _get_disk(),
                "uptime": _get_uptime(),
                "version": _get_version(),
                "robot": _get_robot_status(),
                "camera": _get_camera_status(),
                "recent_commands": list(_command_log),
                **(extra or {}),
            },
        }
        req = urllib.request.Request(
            _REPORT_URL,
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"},
        )
        urllib.request.urlopen(req, timeout=10)
    except Exception:
        pass


def _loop():
    _report("boot")
    while True:
        time.sleep(_INTERVAL)
        _report("heartbeat")


def start():
    if _REPORT_URL:
        t = threading.Thread(target=_loop, daemon=True)
        t.start()
