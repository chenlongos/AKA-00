import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import urllib.request
from flask import Blueprint, request, jsonify

ota_bp = Blueprint("ota", __name__, url_prefix="/api/ota")

APP_DIR = "/root/AKA-00"
OTA_DIR = os.path.join(APP_DIR, ".ota")
VERSION_FILE = os.path.join(APP_DIR, "VERSION")

# ---- 拉取配置 ----
# GitHub Releases API
GITHUB_REPO = os.environ.get("OTA_GITHUB_REPO", "chenlongos/AKA-00")
RELEASE_API = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
# 如果不用 GitHub，可以设置 OTA_CHECK_URL + OTA_DOWNLOAD_URL 两个自定义地址
CUSTOM_CHECK_URL = os.environ.get("OTA_CHECK_URL", "")    # 返回 {"version":"...","url":"..."}
CUSTOM_DOWNLOAD_URL = os.environ.get("OTA_DOWNLOAD_URL", "")
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
    """从远端下载最新固件并安装（拉取模式）"""
    # 先查到最新版本信息
    try:
        info = _fetch_release_info()
    except Exception as e:
        return jsonify({"status": "error", "message": f"无法连接: {e}"}), 502

    if info is None:
        return jsonify({"status": "error", "message": "未找到可用更新"}), 404

    if info["version"] == _version():
        return jsonify({"status": "ok", "message": "已是最新版本", "version": _version()})

    download_url = info["url"]
    if not download_url:
        return jsonify({"status": "error", "message": "固件下载地址为空"}), 500

    # 下载到临时文件
    os.makedirs(OTA_DIR, exist_ok=True)
    tmp_path = os.path.join(OTA_DIR, "download.tmp")

    try:
        _download(download_url, tmp_path)
        _install(tmp_path)
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)

    _restart()
    return jsonify({"status": "ok", "message": "updated, restarting...", "version": info["version"]})


# ============================================================
#  本地上传升级 (push)
# ============================================================

@ota_bp.route("/update", methods=["POST"])
def update():
    """本地上传固件文件并安装"""
    file = request.files.get("firmware")
    if not file or file.filename == "":
        return jsonify({"status": "error", "message": "no firmware file"}), 400

    os.makedirs(OTA_DIR, exist_ok=True)
    tmp_path = os.path.join(OTA_DIR, "upload.tmp")
    file.save(tmp_path)

    expected_md5 = request.form.get("md5", "")
    if expected_md5:
        actual = _md5(tmp_path)
        if actual != expected_md5.lower():
            os.remove(tmp_path)
            return jsonify({"status": "error", "message": f"md5 mismatch: {actual}"}), 400

    try:
        _install(tmp_path)
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)

    _restart()
    return jsonify({"status": "ok", "message": "updated, restarting..."})


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
    with urllib.request.urlopen(req, timeout=DOWNLOAD_TIMEOUT) as resp:
        return json.loads(resp.read().decode())


def _fetch_release_info():
    """
    获取远程版本信息。
    优先使用自定义 URL（OTA_CHECK_URL），否则用 GitHub Releases API。
    返回 {"version": "...", "url": "...", "size": ...} 或 None。
    """
    # 方式 A: 自定义 HTTP 更新源
    if CUSTOM_CHECK_URL:
        data = _http_get_json(CUSTOM_CHECK_URL)
        return {
            "version": str(data.get("version", "")),
            "url": str(data.get("url", CUSTOM_DOWNLOAD_URL)),
            "size": int(data.get("size", 0)),
        }

    # 方式 B: GitHub Releases（默认）
    data = _http_get_json(RELEASE_API)
    tag = data.get("tag_name", "")
    assets = data.get("assets", [])

    # 找名为 aka-ota.tar.gz 的 asset，fallback 到第一个
    download_url = ""
    size = 0
    for a in assets:
        if a.get("name", "").endswith(".tar.gz"):
            download_url = a["browser_download_url"]
            size = a.get("size", 0)
            break
    if not download_url and assets:
        download_url = assets[0]["browser_download_url"]
        size = assets[0].get("size", 0)

    if not download_url:
        return None

    return {"version": tag, "url": download_url, "size": size}


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
    try:
        subprocess.run(
            ["sed", "-n", "/^#__PAYLOAD_BELOW__$/,$p", executable],
            stdout=open(os.path.join(dest, "payload.b64"), "w"),
            check=True,
        )
    except subprocess.CalledProcessError as e:
        raise ValueError(f"payload extract failed: {e}")
    try:
        subprocess.run(
            f"tail -n +2 {dest}/payload.b64 | base64 -d | tar xz -C {dest}",
            shell=True, check=True,
        )
    except subprocess.CalledProcessError as e:
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
