import hashlib
import json
import os
import shutil
import subprocess
import threading
import time
import urllib.request
from flask import Blueprint, request, jsonify
from app.config import config as hw_config

ota_bp = Blueprint("ota", __name__, url_prefix="/api/ota")

APP_DIR = os.path.join(os.path.dirname(__file__), "..", "..")  # app/routes/ota.py → 项目根
OTA_DIR = os.path.join(APP_DIR, ".ota")
VERSION_FILE = os.path.join(APP_DIR, "VERSION")
STATUS_FILE = "/root/aka-ota-status.json"

# 进度追踪（task_id → {progress, status, message}）
_upgrade_tasks = {}


def _write_ota_status(status: str, **extra):
    """持久化 OTA 状态到磁盘，跨后端重启保留。"""
    try:
        data = {"status": status, "timestamp": int(time.time()), **extra}
        with open(STATUS_FILE, "w") as f:
            json.dump(data, f)
    except Exception:
        pass


def _read_ota_status() -> dict:
    """读取持久化的 OTA 状态。"""
    try:
        if os.path.exists(STATUS_FILE):
            with open(STATUS_FILE) as f:
                return json.load(f)
    except Exception:
        pass
    return {"status": "idle"}

# ---- 更新源配置 ----
CHECK_URL = hw_config.ota_check_url or os.environ.get("OTA_CHECK_URL", "")
DOWNLOAD_TIMEOUT = int(os.environ.get("OTA_TIMEOUT", "60"))
CHECK_TIMEOUT = 5  # 检查更新超时（秒）


def _parse_semver(ver: str) -> tuple:
    """解析 git describe 输出为可比较的元组。

    'v1.2.3'              → (1, 2, 3, 0)
    'v1.2.3-4-gabc'       → (1, 2, 3, 4)    ← tag 后 4 个 commit，更新！
    'v1.2.3-dirty'        → (1, 2, 3, 0)
    'abc123'              → ()               ← 无 tag，解析失败
    """
    try:
        s = ver.lstrip("v")
        # Split: "1.2.3" / "1.2.3-4-gabc" / "1.2.3-dirty"
        parts = s.split("-")
        nums = parts[0].split(".")
        major, minor, patch = int(nums[0]), int(nums[1]) if len(nums) > 1 else 0, int(nums[2]) if len(nums) > 2 else 0
        # Commit count after tag: "v1.2.3-4-gabc" → 4; "v1.2.3" → 0
        commits = 0
        if len(parts) > 1 and parts[1].isdigit():
            commits = int(parts[1])
        return (major, minor, patch, commits)
    except (ValueError, IndexError):
        return ()


def _version():
    """返回 (版本号, Unix时间戳)。VERSION 文件格式: 'v1.2.3@1722169200'。"""
    if os.path.exists(VERSION_FILE):
        raw = open(VERSION_FILE).read().strip()
        sep = "@" if "@" in raw else " "
        parts = raw.rsplit(sep, 1) if sep in raw else [raw, raw]
        return parts[0], parts[1]
    return "unknown", "0"


# ============================================================
#  查询
# ============================================================

@ota_bp.route("/version")
def version():
    v, ts = _version()
    return jsonify({"version": v, "updated": int(ts), "service": "AKA-00"})


@ota_bp.route("/upgrade/progress")
def upgrade_progress():
    task_id = request.args.get("task_id", "")
    task = _upgrade_tasks.get(task_id)
    if not task:
        return jsonify({"progress": 0, "status": "unknown"})
    return jsonify(task)


@ota_bp.route("/status")
def ota_status():
    """返回当前 OTA 状态（内存 + 磁盘双重检查）。"""
    # 优先查内存中的活跃任务
    for tid, task in _upgrade_tasks.items():
        if task.get("status") in ("downloading", "installing"):
            return jsonify({"status": task["status"], "progress": task.get("progress", 0),
                            "message": task.get("message", ""), "task_id": tid})
    # 再查磁盘持久化状态
    disk = _read_ota_status()
    return jsonify(disk)


@ota_bp.route("/check")
def check():
    """检查远程是否有新版本（semver 对比）。"""
    try:
        info = _fetch_release_info()
    except Exception as e:
        return jsonify({
            "status": "error",
            "message": f"无法连接更新服务器: {e}",
        }), 502

    if info is None:
        return jsonify({"status": "error", "message": "未找到可用更新"}), 404

    cur_ver, cur_ts = _version()
    remote_ver = info.get("version_number", "").lstrip("v")
    remote_ts = int(info["version"])

    lv = _parse_semver(cur_ver)
    rv = _parse_semver(remote_ver)

    if lv and rv:
        has_update = rv > lv
    else:
        # 版本号无法解析 → 时间戳兜底（兼容旧格式）
        has_update = remote_ts > int(cur_ts or "0")

    return jsonify({
        "current_version": cur_ver,
        "current_updated": int(cur_ts or "0"),
        "remote_updated": remote_ts,
        "update_available": has_update,
        "latest_version": info.get("version_number", ""),
        "hardware_desc": info.get("hardware_desc", ""),
        "software_desc": info.get("software_desc", ""),
        "url": info.get("url", ""),
    })


# ============================================================
#  拉取升级 (pull)
# ============================================================

@ota_bp.route("/upgrade", methods=["POST"])
def upgrade():
    """从远端下载最新固件并安装（拉取模式，异步+进度）"""
    try:
        info = _fetch_release_info()
    except Exception as e:
        return jsonify({"status": "error", "message": f"无法连接更新服务器: {e}"}), 502

    if info is None:
        return jsonify({"status": "error", "message": "未找到可用更新"}), 404

    cur_ver, cur_ts = _version()
    remote_ver = info.get("version_number", "").lstrip("v")
    remote_ts = int(info["version"])

    lv = _parse_semver(cur_ver)
    rv = _parse_semver(remote_ver)
    if lv and rv and rv <= lv:
        return jsonify({"status": "ok", "message": "已是最新版本", "version": cur_ver})
    if not lv and remote_ts <= int(cur_ts or "0"):
        return jsonify({"status": "ok", "message": "已是最新版本", "version": cur_ver})

    download_url = info.get("url", "")
    if not download_url:
        return jsonify({"status": "error", "message": "固件下载地址为空，请检查更新源配置"}), 500

    import uuid
    task_id = uuid.uuid4().hex[:8]
    _upgrade_tasks[task_id] = {"progress": 0, "status": "downloading", "message": "准备下载..."}
    _write_ota_status("downloading", task_id=task_id)

    def _do_upgrade():
        try:
            os.makedirs(OTA_DIR, exist_ok=True)
            tmp_path = os.path.join(OTA_DIR, f"download_{task_id}.tmp")
            _upgrade_tasks[task_id] = {"progress": 0, "status": "downloading", "message": "正在下载固件..."}

            _download_with_progress(download_url, tmp_path, task_id, info.get("size", 0))

            _write_ota_status("installing", task_id=task_id)
            _write_restart_script(tmp_path)
            _upgrade_tasks[task_id] = {"progress": 100, "status": "done", "message": "安装完成，服务重启中..."}
        except Exception as e:
            _upgrade_tasks[task_id] = {"progress": 0, "status": "error", "message": str(e)}
            _write_ota_status("error", task_id=task_id, message=str(e))

    threading.Thread(target=_do_upgrade, daemon=True).start()
    return jsonify({"status": "ok", "task_id": task_id})


# ============================================================
#  本地上传升级 (push)
# ============================================================

@ota_bp.route("/update", methods=["POST"])
def update():
    """本地上传固件文件并安装（异步+进度）"""
    file = request.files.get("firmware")
    if not file or file.filename == "":
        return jsonify({"status": "error", "message": "no firmware file"}), 400

    import uuid
    task_id = uuid.uuid4().hex[:8]

    os.makedirs(OTA_DIR, exist_ok=True)
    tmp_path = os.path.join(OTA_DIR, f"upload_{task_id}.tmp")
    file.save(tmp_path)

    expected_md5 = request.form.get("md5", "")
    if expected_md5:
        actual = _md5(tmp_path)
        if actual != expected_md5.lower():
            os.remove(tmp_path)
            return jsonify({"status": "error", "message": f"md5 mismatch: {actual}"}), 400

    _upgrade_tasks[task_id] = {"progress": 50, "status": "installing", "message": "正在安装固件..."}
    _write_ota_status("installing", task_id=task_id)

    def _do_install():
        try:
            _upgrade_tasks[task_id] = {"progress": 60, "status": "installing", "message": "正在准备..."}
            _write_restart_script(tmp_path)
            _upgrade_tasks[task_id] = {"progress": 100, "status": "done", "message": "安装完成，服务重启中..."}
        except Exception as e:
            _upgrade_tasks[task_id] = {"progress": 0, "status": "error", "message": str(e)}
            _write_ota_status("error", task_id=task_id, message=str(e))

    threading.Thread(target=_do_install, daemon=True).start()
    return jsonify({"status": "ok", "task_id": task_id, "message": "upload received, installing..."})


# ============================================================
#  内部工具
# ============================================================

def _md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def _http_get_json(url, timeout=None):
    """GET 请求，返回解析后的 JSON"""
    if timeout is None:
        timeout = DOWNLOAD_TIMEOUT
    req = urllib.request.Request(url, headers={
        "User-Agent": "AKA-00-OTA/1.0",
        "Accept": "application/json",
    })
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        raise Exception(f"HTTP {e.code}: {e.reason}")


def _fetch_release_info():
    """获取远程版本信息，返回 {"version": "<unix_ts>", "url": "..."} 或 None。"""
    if not CHECK_URL:
        return None
    data = _http_get_json(CHECK_URL, timeout=CHECK_TIMEOUT)

    inner = data.get("data", data)
    url = inner.get("imageUrl", inner.get("url", ""))

    # 用 updatedAt 字段计算 Unix 时间戳
    updated = inner.get("updatedAt", "")
    ts = "0"
    if updated:
        try:
            import datetime
            dt = datetime.datetime.fromisoformat(updated.replace("Z", "+00:00"))
            ts = str(int(dt.timestamp()))
        except Exception:
            ts = "0"

    return {
        "version": ts,
        "version_number": str(inner.get("versionNumber", "")),
        "url": url,
        "hardware_desc": str(inner.get("hardwareDesc", "")),
        "software_desc": str(inner.get("softwareDesc", "")),
    }


def _download(url, dest):
    """流式下载文件到磁盘"""
    req = urllib.request.Request(url, headers={"User-Agent": "AKA-00-OTA/1.0"})
    with urllib.request.urlopen(req, timeout=DOWNLOAD_TIMEOUT) as resp:
        with open(dest, "wb") as f:
            while True:
                chunk = resp.read(65536)
                if not chunk:
                    break
                f.write(chunk)


def _download_with_progress(url, dest, task_id, total_size=0):
    """流式下载，同时更新进度"""
    req = urllib.request.Request(url, headers={"User-Agent": "AKA-00-OTA/1.0"})
    with urllib.request.urlopen(req, timeout=DOWNLOAD_TIMEOUT) as resp:
        # 尝试从响应头获取文件大小
        cl = resp.headers.get("Content-Length")
        size = int(cl) if cl else total_size
        downloaded = 0
        with open(dest, "wb") as f:
            while True:
                chunk = resp.read(65536)
                if not chunk:
                    break
                f.write(chunk)
                downloaded += len(chunk)
                if size > 0:
                    pct = int(downloaded * 100 / size)
                    _upgrade_tasks[task_id] = {
                        "progress": min(pct, 99),
                        "status": "downloading",
                        "message": f"正在下载... {min(pct, 99)}%",
                    }


def _write_restart_script(firmware_path):
    """OTA: 固件是 aka-00-server 自解压脚本，搬移到 /tmp 后 spawn 执行 --update。
    aka-00-server --update 负责解压覆盖 + 重启，不需要 tar.gz + staging + swap。"""
    update_path = "/tmp/aka-ota-update"
    shutil.move(firmware_path, update_path)
    os.chmod(update_path, 0o755)

    script_path = "/tmp/aka-ota-install.sh"
    with open(script_path, "w") as f:
        f.write(f"""#!/bin/sh
set -e
LOCK_FILE="/tmp/aka-ota-lock"

echo "[OTA] acquiring lock..."
touch "$LOCK_FILE"

# Give the HTTP response time to flush before killing Python
sleep 3

echo "[OTA] stopping old Python..."
killall python3 2>/dev/null || true
sleep 2
killall -9 python3 2>/dev/null || true

echo "[OTA] running update..."
exec /tmp/aka-ota-update --update
""")
    os.chmod(script_path, 0o755)
    subprocess.Popen(
        ["/bin/sh", script_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


