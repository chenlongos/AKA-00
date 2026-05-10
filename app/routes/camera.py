from flask import Blueprint, request, jsonify, make_response
import time

from src.state import get_state_collector

camera_bp = Blueprint("camera", __name__, url_prefix="/api/camera")


@camera_bp.route("/open", methods=["POST"])
def camera_open():
    from app.services.camera_service import CameraService
    service = CameraService.get_instance()
    service._ensure_camera()
    available = service.is_available()
    return jsonify({"camera_on": available})


@camera_bp.route("/close", methods=["POST"])
def camera_close():
    from app.services.camera_service import CameraService
    CameraService.get_instance().close()
    return jsonify({"camera_on": False})


@camera_bp.route("/status")
def camera_status():
    from app.services.camera_service import CameraService
    available = CameraService.get_instance().is_available()
    return jsonify({"camera_on": available})


@camera_bp.route("/stream")
def video_stream():
    """MJPEG视频流 - 直接从Camera读取最新帧，无竞争"""
    import cv2
    from src.state import get_state_collector

    collector = get_state_collector()

    if collector._camera is None or not hasattr(collector._camera, '_frame_ready'):
        return jsonify({"error": "camera not available"}), 500

    def generate():
        try:
            while True:
                if not collector._running:
                    break
                # 直接从 Camera 读最新帧引用，无 event 竞争
                ret, frame = collector._camera.read()
                if not ret or frame is None:
                    time.sleep(0.05)
                    continue
                ret, jpg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 40])
                if not ret:
                    continue
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n\r\n' + jpg.tobytes() + b'\r\n')
                time.sleep(0.03)  # ~30fps 上限
        except GeneratorExit:
            pass

    response = make_response(generate())
    response.headers['Content-Type'] = 'multipart/x-mixed-replace; boundary=frame'
    response.headers['Cache-Control'] = 'no-cache, no-store, must-revalidate'
    response.headers['Pragma'] = 'no-cache'
    response.headers['Expires'] = '0'
    return response


@camera_bp.route("/all_status")
def all_status():
    import cv2
    import base64

    collector = get_state_collector()
    status = collector.get_status()
    image = collector.get_image()

    image_data = None
    if image is not None:
        ret, jpg = cv2.imencode('.jpg', image, [cv2.IMWRITE_JPEG_QUALITY, 25])
        if ret:
            image_data = base64.b64encode(jpg.tobytes()).decode('ascii')

    return jsonify({
        "timestamp": request.args.get("timestamp"),
        "left_speed": status.left_speed,
        "right_speed": status.right_speed,
        "left_action": status.left_action,
        "right_action": status.right_action,
        "timestamp_ms": status.timestamp_ms,
        "image": image_data,
        "image_format": "jpeg",
    })