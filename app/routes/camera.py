from flask import Blueprint, request, jsonify, make_response
import base64
import cv2
import numpy as np
import time

from src.state import get_state_collector
from app.services.camera_service import CameraService
from app.config import config

camera_bp = Blueprint("camera", __name__, url_prefix="/api/camera")


@camera_bp.route("/snapshot")
def snapshot():
    """返回 320x240 图片 + 小车自己的 m 和 c

    Returns:
        {"image": "<base64 jpeg>", "width": 320, "height": 240,
         "m": 3062.0, "c": -5.0}
    """
    service = CameraService.get_instance()
    service._ensure_camera()
    ret, frame = service.read()
    if not ret or frame is None:
        return jsonify({"error": "camera not available"}), 500

    # letterbox: 等比缩放 + 黑边填充到 320x240，保证画面不变形
    H, W = frame.shape[:2]
    r = min(320.0 / W, 240.0 / H)
    new_w, new_h = int(round(W * r)), int(round(H * r))
    resized = cv2.resize(frame, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
    canvas = np.zeros((240, 320, 3), dtype=np.uint8)
    dw = (320 - new_w) // 2
    dh = (240 - new_h) // 2
    canvas[dh:dh + new_h, dw:dw + new_w] = resized
    frame_out = canvas
    ret, jpg = cv2.imencode(".jpg", frame_out, [cv2.IMWRITE_JPEG_QUALITY, 70])
    if not ret:
        return jsonify({"error": "encode failed"}), 500

    return jsonify({
        "image": base64.b64encode(jpg.tobytes()).decode("ascii"),
        "width": 320,
        "height": 240,
        "format": "jpeg",
        "m": config.calib_m,
        "c": config.calib_c,
    })


@camera_bp.route("/open", methods=["POST"])
def camera_open():
    service = CameraService.get_instance()
    service._ensure_camera()
    available = service.is_available()
    return jsonify({"camera_on": available})


@camera_bp.route("/close", methods=["POST"])
def camera_close():
    CameraService.get_instance().close()
    return jsonify({"camera_on": False})


@camera_bp.route("/status")
def camera_status():
    available = CameraService.get_instance().is_available()
    return jsonify({"camera_on": available})


@camera_bp.route("/stream")
def video_stream():
    """MJPEG视频流 - 直接从Camera读取最新帧，无竞争"""
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
    response.headers['Access-Control-Allow-Origin'] = '*'
    response.headers['Cache-Control'] = 'no-cache, no-store, must-revalidate'
    response.headers['Pragma'] = 'no-cache'
    response.headers['Expires'] = '0'
    return response


@camera_bp.route("/speed")
def speed_status():
    """只获取左右轮速度和目标值"""
    collector = get_state_collector()
    status = collector.get_status()
    return jsonify({
        "left_speed": status.left_speed,
        "right_speed": status.right_speed,
        "left_target": status.left_target,
        "right_target": status.right_target,
        "gripper_status": status.gripper_status,
        "gripper_target": status.gripper_target,
        "timestamp_ms": status.timestamp_ms,
    })


@camera_bp.route("/all_status")
def all_status():
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
        "left_target": status.left_target,
        "right_target": status.right_target,
        "gripper_status": status.gripper_status,
        "gripper_target": status.gripper_target,
        "timestamp_ms": status.timestamp_ms,
        "image": image_data,
        "image_format": "jpeg",
    })