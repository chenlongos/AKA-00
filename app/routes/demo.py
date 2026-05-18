import os
import signal
import urllib.request
import threading
from flask import Blueprint, request, jsonify

demo_bp = Blueprint("demo", __name__, url_prefix="/api/demo")


@demo_bp.route("/init", methods=["POST"])
def demo_init():
    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body is required"}), 400

    demo_name = payload.get("name")
    if not isinstance(demo_name, str) or not demo_name:
        return jsonify({"error": "name is required"}), 400

    base_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__))), "demo")
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
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
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


@demo_bp.route("/download_model", methods=["POST"])
def demo_download_model():
    """从 demo-server 下载模型到 demo/<name>/ 目录"""
    payload = request.get_json(silent=True)
    if not payload:
        return jsonify({"error": "json body is required"}), 400

    demo_name = payload.get("demo_name")
    model_name = payload.get("model_name")
    demo_server = payload.get("demo_server", "http://localhost:8888")

    if not demo_name or not model_name:
        return jsonify({"error": "demo_name and model_name are required"}), 400

    base_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__))), "demo")
    demo_dir = os.path.join(base_dir, demo_name)

    if not os.path.isdir(demo_dir):
        return jsonify({"error": f"demo '{demo_name}' not found"}), 404

    file_path = os.path.join(demo_dir, model_name)
    url = f"{demo_server}/api/models/{model_name}"

    try:
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

        return jsonify({
            "status": "downloaded",
            "demo_name": demo_name,
            "model_name": model_name,
            "path": file_path,
            "size": total_size,
        })
    except Exception as exc:
        return jsonify({"error": str(exc)}), 500


# 存储下载进度的全局字典
_download_progress = {}


def _do_download(task_id, url, file_path):
    """后台下载线程"""
    try:
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
    """带进度的下载，客户端轮询进度"""
    payload = request.get_json(silent=True)
    if not payload:
        return jsonify({"error": "json body is required"}), 400

    demo_name = payload.get("demo_name")
    model_name = payload.get("model_name")
    demo_server = payload.get("demo_server", "http://localhost:8888")

    if not demo_name or not model_name:
        return jsonify({"error": "demo_name and model_name are required"}), 400

    base_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__))), "demo")
    demo_dir = os.path.join(base_dir, demo_name)

    if not os.path.isdir(demo_dir):
        return jsonify({"error": f"demo '{demo_name}' not found"}), 404

    task_id = f"{demo_name}_{model_name}"
    file_path = os.path.join(demo_dir, model_name)
    url = f"{demo_server}/api/models/{model_name}"

    _download_progress[task_id] = {"progress": 0, "status": "downloading", "error": None}

    # 后台线程下载
    thread = threading.Thread(target=_do_download, args=(task_id, url, file_path))
    thread.daemon = True
    thread.start()

    return jsonify({
        "status": "started",
        "task_id": task_id,
    })


@demo_bp.route("/download_progress/<task_id>", methods=["GET"])
def demo_get_download_progress(task_id):
    """获取下载进度"""
    if task_id in _download_progress:
        return jsonify(_download_progress[task_id])
    return jsonify({"progress": 0, "status": "not_found"})