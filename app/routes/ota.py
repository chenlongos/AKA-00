import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import threading
import urllib.request
from flask import Blueprint, request, jsonify
from app.config import config as hw_config

ota_bp = Blueprint("ota", __name__, url_prefix="/api/ota")

APP_DIR = "/root/AKA-00"
OTA_DIR = os.path.join(APP_DIR, ".ota")
VERSION_FILE = os.path.join(APP_DIR, "VERSION")

# 进度追踪（task_id → {progress, status, message}）
_upgrade_tasks = {}

# ---- 更新源配置 ----
CHECK_URL = hw_config.ota_check_url or os.environ.get("OTA_CHECK_URL", "")
DOWNLOAD_URL = hw_config.ota_download_url or os.environ.get("OTA_DOWNLOAD_URL", "")
DOWNLOAD_TIMEOUT = int(os.environ.get("OTA_TIMEOUT", "60"))


def _version():
    if os.path.exists(VERSION_FILE):
        with open(VERSION_FILE) as f:
            return f.read().strip()
    return "unknown"


# ============================================================
#  查询
# ============================================================

@ota_bp.route("/version")
def version():
    return jsonify({"version": _version(), "service": "AKA-00"})


@ota_bp.route("/upgrade/progress")
def upgrade_progress():
    task_id = request.args.get("task_id", "")
    task = _upgrade_tasks.get(task_id)
    if not task:
        return jsonify({"progress": 0, "status": "unknown"})
    return jsonify(task)


@ota_bp.route("/check")
def check():
    """检查远程是否有新版本"""
    current = _version()
    try:
        info = _fetch_release_info()
    except Exception as e:
        return jsonify({
            "status": "error",
            "message": f"无法连接更新服务器: {e}",
            "current": current,
        }), 502

    if info is None:
        return jsonify({
            "status": "error",
            "message": "未找到可用更新",
            "current": current,
        }), 404

    latest = info["version"]
    has_update = latest != current

    return jsonify({
        "current": current,
        "latest": latest,
        "update_available": has_update,
        "size": info.get("size", 0),
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

    if info.get("version") == _version():
        return jsonify({"status": "ok", "message": "已是最新版本", "version": _version()})

    download_url = info.get("url", "")
    if not download_url:
        return jsonify({"status": "error", "message": "固件下载地址为空，请检查更新源配置"}), 500

    import uuid
    task_id = uuid.uuid4().hex[:8]
    _upgrade_tasks[task_id] = {"progress": 0, "status": "downloading", "message": "准备下载..."}

    def _do_upgrade():
        try:
            os.makedirs(OTA_DIR, exist_ok=True)
            tmp_path = os.path.join(OTA_DIR, f"download_{task_id}.tmp")
            total_size = info.get("size", 0)

            _upgrade_tasks[task_id] = {"progress": 0, "status": "downloading", "message": "正在下载固件..."}
            _download_with_progress(download_url, tmp_path, task_id, total_size)

            _upgrade_tasks[task_id] = {"progress": 100, "status": "installing", "message": "正在安装..."}
            _install(tmp_path)

            if os.path.exists(tmp_path):
                os.remove(tmp_path)

            _upgrade_tasks[task_id] = {"progress": 100, "status": "done", "message": "安装完成，即将重启"}
            _restart()
        except Exception as e:
            _upgrade_tasks[task_id] = {"progress": 0, "status": "error", "message": str(e)}

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

    def _do_install():
        try:
            _upgrade_tasks[task_id] = {"progress": 60, "status": "installing", "message": "正在解压..."}
            _install(tmp_path)

            if os.path.exists(tmp_path):
                os.remove(tmp_path)

            _upgrade_tasks[task_id] = {"progress": 100, "status": "done", "message": "安装完成，即将重启"}
            _restart()
        except Exception as e:
            _upgrade_tasks[task_id] = {"progress": 0, "status": "error", "message": str(e)}

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


def _http_get_json(url):
    """GET 请求，返回解析后的 JSON"""
    req = urllib.request.Request(url, headers={
        "User-Agent": "AKA-00-OTA/1.0",
        "Accept": "application/json",
    })
    try:
        with urllib.request.urlopen(req, timeout=DOWNLOAD_TIMEOUT) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        raise Exception(f"HTTP {e.code}: {e.reason}")


def _fetch_release_info():
    """获取远程版本信息，返回 {"version": "...", "url": "...", "size": ...} 或 None。"""
    if not CHECK_URL:
        return None
    data = _http_get_json(CHECK_URL)
    return {
        "version": str(data.get("version", "")),
        "url": str(data.get("url", DOWNLOAD_URL)),
        "size": int(data.get("size", 0)),
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


def _install(path):
    staging = os.path.join(OTA_DIR, "staging")
    if os.path.exists(staging):
        shutil.rmtree(staging)
    os.makedirs(staging)

    # 判断格式
    with open(path, "rb") as f:
        magic = f.read(2)

    if magic == b"\x1f\x8b":
        # 标准 tar.gz
        with tarfile.open(path, mode="r:gz") as tar:
            tar.extractall(path=staging)
    else:
        # 自解压 aka-server
        _extract_selfext(path, staging)

    if not os.path.exists(os.path.join(staging, "run.py")):
        raise ValueError("invalid firmware: run.py not found")

    # 覆盖安装
    for item in os.listdir(staging):
        src = os.path.join(staging, item)
        dst = os.path.join(APP_DIR, item)
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        elif os.path.exists(dst) or os.path.islink(dst):
            os.remove(dst)
        if os.path.isdir(src):
            shutil.copytree(src, dst, symlinks=True, ignore_dangling_symlinks=True)
        else:
            shutil.copy2(src, dst)

    # 确保脚本可执行
    for name in ["init.sh", "uart_init.sh", "pwm_init.sh"]:
        p = os.path.join(APP_DIR, name)
        if os.path.exists(p):
            os.chmod(p, 0o755)

    shutil.rmtree(staging)


def _extract_selfext(executable, dest):
    import base64
    try:
        subprocess.run(
            ["sed", "-n", "/^#__PAYLOAD_BELOW__$/,$p", executable],
            stdout=open(os.path.join(dest, "payload.b64"), "w"),
            check=True,
        )
    except subprocess.CalledProcessError as e:
        raise ValueError(f"payload extract failed (sed): {e}")

    # 用 Python 解码 base64 再解压 tar，避免依赖系统 base64 命令
    try:
        b64_path = os.path.join(dest, "payload.b64")
        tar_path = os.path.join(dest, "payload.tar.gz")
        with open(b64_path) as f:
            f.readline()  # 跳过 #__PAYLOAD_BELOW__ 行
            encoded = f.read()
        with open(tar_path, "wb") as f:
            f.write(base64.b64decode(encoded))
        with tarfile.open(tar_path, mode="r:gz") as tar:
            tar.extractall(path=dest)
        os.remove(b64_path)
        os.remove(tar_path)
    except Exception as e:
        raise ValueError(f"decode failed: {e}")


def _restart():
    script = os.path.join(OTA_DIR, "restart.sh")
    with open(script, "w") as f:
        f.write("""#!/bin/sh
sleep 2
kill $(pgrep -f "python3.*run.py") 2>/dev/null
sleep 1
cd /root/AKA-00 && exec ./init.sh
""")
    os.chmod(script, 0o755)
    subprocess.Popen(
        ["/bin/sh", script],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
