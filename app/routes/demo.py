import os
import signal
import shutil
import urllib.request
import threading
from flask import Blueprint, request, jsonify
from app.config import config

demo_bp = Blueprint("demo", __name__, url_prefix="/api/demo")


def _get_base_dir():
    return os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__))), "demo")


def _find_current_demo():
    """找到 demo/ 下唯一的 demo 目录（含 init.sh），返回目录名或 None"""
    base_dir = _get_base_dir()
    if not os.path.isdir(base_dir):
        return None
    for name in sorted(os.listdir(base_dir)):
        demo_dir = os.path.join(base_dir, name)
        if os.path.isdir(demo_dir) and os.path.isfile(os.path.join(demo_dir, "init.sh")):
            return name
    return None


def _list_all_demos():
    """列出 demo/ 下所有包含 init.sh 的目录信息（与 dora/web-server/services/demo.rs 对齐）"""
    base_dir = _get_base_dir()
    if not os.path.isdir(base_dir):
        return []
    demos = []
    for name in sorted(os.listdir(base_dir)):
        demo_dir = os.path.join(base_dir, name)
        if os.path.isdir(demo_dir) and os.path.isfile(os.path.join(demo_dir, "init.sh")):
            demos.append({
                "name": name,
                "path": demo_dir,
                "kind": "binary",  # python mode 只认 init.sh，所以全是 binary
            })
    return demos


@demo_bp.route("/list", methods=["GET"])
def demo_list():
    """返回所有可用的本地 demo 目录（结构与 Rust web-server 对齐：[{name, path, kind}, ...]）"""
    return jsonify({"demos": _list_all_demos()})


@demo_bp.route("/name", methods=["GET"])
def demo_name():
    """返回当前本地 demo 名称"""
    return jsonify({"name": _find_current_demo()})


@demo_bp.route("/init", methods=["POST"])
def demo_init():
    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body is required"}), 400

    demo_name = payload.get("name") or _find_current_demo()
    if not demo_name:
        return jsonify({"error": "no demo found"}), 404

    base_dir = _get_base_dir()
    demo_dir = os.path.join(base_dir, demo_name)
    init_script = os.path.join(demo_dir, "init.sh")

    if not os.path.isdir(demo_dir):
        return jsonify({"error": f"demo '{demo_name}' not found"}), 404

    if not os.path.isfile(init_script):
        return jsonify({"error": f"init.sh not found in demo '{demo_name}'"}), 404

    if hasattr(demo_init, "_process") and demo_init._process is not None:
        pid = demo_init._process.pid
        try:
            os.kill(pid, 0)
            return jsonify({"error": "demo is already running", "pid": pid}), 409
        except OSError:
            demo_init._process = None
            demo_init._name = None

    os.chmod(init_script, 0o755)

    import subprocess
    try:
        proc = subprocess.Popen(
            [init_script],
            cwd=demo_dir,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        demo_init._process = proc
        demo_init._name = demo_name
        demo_init._pgid = os.getpgid(proc.pid)
        return jsonify({
            "status": "started",
            "pid": proc.pid,
            "pgid": demo_init._pgid,
            "name": demo_name,
        })
    except Exception as exc:
        return jsonify({"error": str(exc)}), 500


@demo_bp.route("/stop", methods=["POST"])
def demo_stop():
    if not hasattr(demo_init, "_process") or demo_init._process is None:
        return jsonify({
            "status": "already_stopped",
            "name": getattr(demo_init, "_name", None) or "unknown",
        })

    proc = demo_init._process
    demo_init._process = None
    demo_init._name = None

    pid = proc.pid
    os.kill(pid, signal.SIGTERM)
    try:
        proc.wait(timeout=3)
    except Exception:
        pass

    return jsonify({
        "status": "stopped",
        "pid": pid,
    })


# 存储下载进度的全局字典
_download_progress = {}


def _do_download(task_id, url, file_path, new_demo_dir, old_demo_dir):
    """后台下载线程：下载模型，必要时先重命名目录"""
    try:
        # 如果新 demo 名称不同，先重命名目录
        if new_demo_dir != old_demo_dir and os.path.isdir(old_demo_dir):
            # 如果目标已存在（下载过同样的模型），先删掉旧的
            if os.path.isdir(new_demo_dir):
                shutil.rmtree(new_demo_dir)
            os.rename(old_demo_dir, new_demo_dir)

        with urllib.request.urlopen(url) as response:
            total_size = int(response.headers.get("Content-Length", 0))
            downloaded = 0

            with open(file_path, 'wb') as f:
                while True:
                    chunk = response.read(8192)
                    if not chunk:
                        break
                    f.write(chunk)
                    downloaded += len(chunk)
                    if total_size > 0:
                        _download_progress[task_id]["progress"] = int(downloaded * 100 / total_size)

        _download_progress[task_id]["progress"] = 100
        _download_progress[task_id]["status"] = "done"
    except Exception as exc:
        _download_progress[task_id]["status"] = "error"
        _download_progress[task_id]["error"] = str(exc)


@demo_bp.route("/download_model_with_progress", methods=["POST"])
def demo_download_model_with_progress():
    """下载模型到当前 demo 目录，必要时重命名目录以匹配模型名称"""
    payload = request.get_json(silent=True)
    if not payload:
        return jsonify({"error": "json body is required"}), 400

    model_name = payload.get("model_name")
    demo_server = payload.get("demo_server", config.demo_server_url)

    if not model_name:
        return jsonify({"error": "model_name is required"}), 400

    current_name = _find_current_demo()
    if not current_name:
        return jsonify({"error": "no local demo found"}), 404

    base_dir = _get_base_dir()
    old_dir = os.path.join(base_dir, current_name)
    new_dir = os.path.join(base_dir, model_name)

    # 下载的模型文件统一命名为 yolo_model.cvimodel
    file_path = os.path.join(new_dir if model_name != current_name else old_dir, "yolo_model.cvimodel")
    url = f"{demo_server}/api/models/{model_name}"

    task_id = f"{current_name}_to_{model_name}"
    _download_progress[task_id] = {"progress": 0, "status": "downloading", "error": None}

    thread = threading.Thread(target=_do_download, args=(task_id, url, file_path, new_dir, old_dir))
    thread.daemon = True
    thread.start()

    return jsonify({
        "status": "started",
        "task_id": task_id,
        "new_name": model_name,
    })


@demo_bp.route("/upload_model", methods=["POST"])
def demo_upload_model():
    """直接接收模型文件上传（前端转发，无需 demo_server URL）"""
    if 'file' not in request.files:
        return jsonify({"error": "file is required"}), 400

    file = request.files['file']
    if file.filename == '':
        return jsonify({"error": "empty filename"}), 400

    current_name = _find_current_demo()
    if not current_name:
        return jsonify({"error": "no local demo found"}), 404

    base_dir = _get_base_dir()
    demo_dir = os.path.join(base_dir, current_name)
    file_path = os.path.join(demo_dir, "yolo_model.cvimodel")

    try:
        file.save(file_path)
        file_size = os.path.getsize(file_path)
        return jsonify({"status": "uploaded", "size": file_size, "name": current_name})
    except Exception as exc:
        return jsonify({"error": str(exc)}), 500


@demo_bp.route("/download_progress/<task_id>", methods=["GET"])
def demo_get_download_progress(task_id):
    """获取下载进度"""
    if task_id in _download_progress:
        return jsonify(_download_progress[task_id])
    return jsonify({"progress": 0, "status": "not_found"})
