import os
import signal
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