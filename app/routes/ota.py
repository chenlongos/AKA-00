import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import time
import threading
import urllib.request
from flask import Blueprint, request, jsonify
from app.config import config as hw_config

ota_bp = Blueprint("ota", __name__, url_prefix="/api/ota")

APP_DIR = os.path.join(os.path.dirname(__file__), "..", "..")  # app/routes/ota.py → 项目根
OTA_DIR = os.path.join(APP_DIR, ".ota")
VERSION_FILE = os.path.join(APP_DIR, "VERSION")

# 进度追踪（task_id → {progress, status, message}）
_upgrade_tasks = {}

# ---- 更新源配置 ----
CHECK_URL = hw_config.ota_check_url or os.environ.get("OTA_CHECK_URL", "")
DOWNLOAD_TIMEOUT = int(os.environ.get("OTA_TIMEOUT", "60"))
CHECK_TIMEOUT = 5  # 检查更新超时（秒）


def _version():
    """返回 (版本号, Unix时间戳)。支持空格或 @ 分隔。"""
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


@ota_bp.route("/check")
def check():
    """检查远程是否有新版本（时间戳对比）"""
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

    # 版本号相同 → 只同步时间戳，不需要更新
    if remote_ver and cur_ver.lstrip("v") == remote_ver:
        if remote_ts > int(cur_ts or "0"):
            try:
                with open(VERSION_FILE, "w") as f:
                    f.write(f"{cur_ver} {remote_ts}")
                print(f"[ota] synced local timestamp to {remote_ts}", flush=True)
            except Exception as e:
                print(f"[ota] sync timestamp failed: {e}", flush=True)
        has_update = False
    else:
        has_update = remote_ts > int(cur_ts)
    print(f"[ota] local={cur_ver}@{cur_ts} remote={remote_ver}@{remote_ts} update={has_update}", flush=True)

    return jsonify({
        "current_version": cur_ver,
        "current_updated": int(cur_ts),
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
            _upgrade_tasks[task_id] = {"progress": 0, "status": "downloading", "message": "正在下载固件..."}
            _download_with_progress(download_url, tmp_path, task_id, info.get("size", 0))

            # 写重启脚本（shell 做原子替换，Python 不碰自己的文件）
            _write_restart_script(tmp_path)
            _upgrade_tasks[task_id] = {"progress": 100, "status": "done", "message": "安装完成，即将重启"}
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
            _upgrade_tasks[task_id] = {"progress": 60, "status": "installing", "message": "正在准备..."}
            _write_restart_script(tmp_path)
            _upgrade_tasks[task_id] = {"progress": 100, "status": "done", "message": "安装完成，即将重启"}
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
    """写 shell 安装脚本——由 shell 停进程、解压、替换、重启"""
    script_path = os.path.join(OTA_DIR, "install.sh")
    with open(script_path, "w") as f:
        f.write(f"""#!/bin/sh
set -e
echo "[OTA] stopping old process..."
kill $(pgrep -f "python3.*run.py") 2>/dev/null || true
sleep 2

echo "[OTA] extracting..."
rm -rf {OTA_DIR}/staging
mkdir -p {OTA_DIR}/staging

MAGIC=$(head -c2 "{firmware_path}" 2>/dev/null)
if [ "$MAGIC" = "$(printf '\\x1f\\x8b')" ]; then
    tar xzf "{firmware_path}" -C {OTA_DIR}/staging
else
    sed -n '/^#__PAYLOAD_BELOW__$/,$p' "{firmware_path}" | tail -n +2 | python3 -c "
import base64, sys, tarfile, tempfile, os
tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.tar.gz')
tmp.write(base64.b64decode(sys.stdin.buffer.read()))
tmp.close()
tarfile.open(tmp.name, mode='r:gz').extractall(path='{OTA_DIR}/staging')
os.unlink(tmp.name)
"
fi

# 进入可能的子目录
if [ -d {OTA_DIR}/staging/*/ ] 2>/dev/null; then
    cd {OTA_DIR}/staging/*/
else
    cd {OTA_DIR}/staging
fi

echo "[OTA] installing..."
for item in * .[!.]*; do
    [ "$item" = "." ] && continue
    [ "$item" = ".." ] && continue
    [ ! -e "$item" ] && continue
    dst="{APP_DIR}/$item"
    if [ -d "$dst" ]; then rm -rf "$dst"; fi
    cp -r "$item" "$dst"
done

chmod +x {APP_DIR}/*.sh 2>/dev/null || true
rm -rf {OTA_DIR}/staging
rm -f "{firmware_path}"

echo "[OTA] restarting..."
cd {APP_DIR} && exec ./init.sh
""")
    os.chmod(script_path, 0o755)
    subprocess.Popen(
        ["/bin/sh", script_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


