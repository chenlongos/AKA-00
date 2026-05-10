import os
import signal
import socket
import struct

from src.state import get_state_collector

try:
    import fcntl
    _HAS_FCNTL = True
except Exception:
    _HAS_FCNTL = False

from flask import Blueprint, request, jsonify, make_response

from app.config import config
from app.services import get_control_service
from src.arm_control.angle_config import load_arm_angles, save_arm_angles
from src.base_control.pwm_channel_config import save_pwm_channels

api_bp = Blueprint("api", __name__)


@api_bp.route("/ip")
def ip():
    return jsonify({
        "ip": get_ip("wlan0")
    })


@api_bp.route("/motor_status")
def motor_status():
    """获取小车左右轮当前状态。"""
    return jsonify(get_state_collector().get_state())


@api_bp.route('/control', methods=['GET'])
def control():
    action = request.args.get('action')
    speed = int(request.args.get('speed', 50))
    milliseconds = float(request.args.get('time', 0))

    try:
        return jsonify(get_control_service().execute_action(action, speed, milliseconds))
    except ValueError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400


@api_bp.route("/arm_angles", methods=["GET", "POST"])
def arm_angles():
    driver = config.arm_driver

    if request.method == "GET":
        return jsonify({
            "driver": driver,
            "angles": load_arm_angles(driver),
        })

    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body is required"}), 400

    body_driver = payload.get("driver", driver)
    if body_driver != driver:
        return jsonify({"error": f"driver mismatch: expected {driver}, got {body_driver}"}), 400

    angles_payload = payload.get("angles", payload)

    try:
        angles = save_arm_angles(driver, angles_payload)
        get_control_service().update_arm_angles(driver, angles)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400

    return jsonify({
        "status": "success",
        "driver": driver,
        "angles": angles,
    })


@api_bp.route("/arm_angles/preview", methods=["POST"])
def arm_angles_preview():
    driver = config.arm_driver
    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body is required"}), 400

    body_driver = payload.get("driver", driver)
    if body_driver != driver:
        return jsonify({"error": f"driver mismatch: expected {driver}, got {body_driver}"}), 400

    key = payload.get("key")
    value = payload.get("value")
    angles_payload = payload.get("angles")

    if not isinstance(key, str):
        return jsonify({"error": "key is required"}), 400
    if not isinstance(angles_payload, dict):
        return jsonify({"error": "angles is required"}), 400

    try:
        angle_value = int(value)
        angles = save_arm_angles(driver, angles_payload)
        service = get_control_service()
        service.update_arm_angles(driver, angles)
        service.preview_arm_angle(driver, key, angle_value)
    except ValueError as exc:
        return jsonify({"error": str(exc)}), 400

    return jsonify({
        "status": "success",
        "driver": driver,
        "key": key,
        "value": angle_value,
        "angles": angles,
    })


@api_bp.route("/base_pwm_channels", methods=["GET", "POST"])
def base_pwm_channels():
    if request.method == "GET":
        return jsonify({
            "pwm_channels": get_control_service().get_pwm_channels(),
        })

    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body is required"}), 400

    pwm_channels_payload = payload.get("pwm_channels", payload)
    pwm_channels = save_pwm_channels(payload=pwm_channels_payload)
    get_control_service().update_pwm_channels(pwm_channels)

    return jsonify({
        "status": "success",
        "pwm_channels": pwm_channels,
    })


def get_ip(ifname="wlan0"):
    if not _HAS_FCNTL:
        return socket.gethostbyname(socket.gethostname())
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    return socket.inet_ntoa(
        fcntl.ioctl(
            s.fileno(),
            0x8915,
            struct.pack('256s', ifname[:15].encode('utf-8'))
        )[20:24]
    )

@api_bp.route('/raw_command', methods=['GET'])
def raw_command():
    cmd = request.args.get('cmd', '')
    return jsonify(get_control_service().send_raw_command(cmd))


@api_bp.route('/motor_direct', methods=['GET'])
def motor_direct():
    """直接设置左右轮速度和运动的持续时间

    Query params:
        left: int    左轮速度 (-255 ~ 255)
        right: int   右轮速度 (-255 ~ 255)
        duration: float 持续时间（秒），0 表示无限
    """
    left = int(float(request.args.get('left', 0)))
    right = int(float(request.args.get('right', 0)))
    duration = float(request.args.get('duration', 0))

    try:
        result = get_control_service().run_motor(left, right, duration)
        # 同时返回当前速度，减少请求次数
        from src.state import get_state_collector
        status = get_state_collector().get_state()
        result["left_speed"] = status.get("left_speed", 0)
        result["right_speed"] = status.get("right_speed", 0)
        return jsonify(result)
    except Exception as e:
        import traceback
        traceback.print_exc()
        return jsonify({"error": str(e)}), 500


@api_bp.route("/heartbeat")
def heartbeat():
    """心跳检测API，用于检查服务是否存活。"""
    mac_address = get_mac_address("wlan0")
    return jsonify({
        "status": "ok",
        "service": "AKA-00",
        "mac_address": mac_address
    })


def get_mac_address(ifname="wlan0"):
    """获取指定网络接口的MAC地址。"""
    try:
        with open(f"/sys/class/net/{ifname}/address", "r") as f:
            return f.read().strip()
    except Exception:
        return "unknown"


@api_bp.route("/demo/init", methods=["POST"])
def demo_init():
    """启动 demo 进程，可通过 /demo/stop 停止"""
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
            # process is dead, clear stale reference
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


@api_bp.route("/demo/stop", methods=["POST"])
def demo_stop():
    """立即停止当前运行的 demo 进程"""
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


@api_bp.route("/video_stream")
def video_stream():
    """MJPEG视频流"""
    try:
        import cv2
        from app.services.camera_service import CameraService

        camera_service = CameraService.get_instance()

        if not camera_service.is_available():
            return jsonify({"error": "camera not available"}), 500

        def generate():
            try:
                while True:
                    ret, frame = camera_service.read()
                    if not ret:
                        break
                    # WebP编码：比JPEG小30-50%，传输更快
                    ret, webp = cv2.imencode('.webp', frame, [cv2.IMWRITE_WEBP_QUALITY, 20])
                    if not ret:
                        continue
                    yield (b'--frame\r\n'
                           b'Content-Type: image/webp\r\n\r\n' + webp.tobytes() + b'\r\n')
            except GeneratorExit:
                pass

        response = make_response(generate())
        response.headers['Content-Type'] = 'multipart/x-mixed-replace; boundary=frame'
        response.headers['Cache-Control'] = 'no-cache, no-store, must-revalidate'
        response.headers['Pragma'] = 'no-cache'
        response.headers['Expires'] = '0'
        return response
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@api_bp.route("/camera", methods=["POST"])
def camera_control():
    """打开或关闭摄像头

    Body:
        action: "open" | "close"
    """
    from app.services.camera_service import CameraService

    payload = request.get_json(silent=True)
    if not isinstance(payload, dict):
        return jsonify({"error": "json body is required"}), 400

    action = payload.get("action")
    if action not in ("open", "close"):
        return jsonify({"error": "action must be 'open' or 'close'"}), 400

    service = CameraService.get_instance()

    if action == "open":
        service._ensure_camera()
        available = service.is_available()
    else:
        service.close()
        available = False

    return jsonify({"camera_on": available})


@api_bp.route("/camera/status")
def camera_status():
    """获取摄像头状态"""
    from app.services.camera_service import CameraService
    available = CameraService.get_instance().is_available()
    return jsonify({"camera_on": available})


@api_bp.route("/video_stream/close", methods=["POST"])
def video_stream_close():
    """关闭视频流，释放摄像头"""
    try:
        from app.services.camera_service import CameraService
        CameraService.get_instance().close()
        return jsonify({"status": "ok"})
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@api_bp.route("/all_status")
def all_status():
    """获取所有状态：电机速度、动作、图像（WebP编码用于最快传输）"""
    import cv2
    import base64

    collector = get_state_collector()

    # 获取状态和动作
    state = collector.get_state()
    action = collector.get_action()

    # 获取图像并编码为WebP（比JPEG小30-50%）
    image = collector.get_image()
    image_data = None
    if image is not None:
        # WebP: quality 0 = 最小文件, 抓换最快
        ret, webp = cv2.imencode('.webp', image, [cv2.IMWRITE_WEBP_QUALITY, 10])
        if ret:
            image_data = base64.b64encode(webp.tobytes()).decode('ascii')

    return jsonify({
        "timestamp": request.args.get("timestamp"),
        "state": state,
        "action": action,
        "image": image_data,
        "image_format": "webp",
    })
