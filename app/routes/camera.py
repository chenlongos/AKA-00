from flask import Blueprint, request, jsonify, make_response

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


@camera_bp.route("/all_status")
def all_status():
    import cv2
    import base64

    collector = get_state_collector()
    state = collector.get_state()
    action = collector.get_action()
    image = collector.get_image()
    image_data = None
    if image is not None:
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