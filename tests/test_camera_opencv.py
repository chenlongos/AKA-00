"""测试 OpenCV 摄像头视频流，带分辨率、帧大小和时间戳标记。

用法:
    python test_camera_opencv.py --device 0 --width 640 --height 480 --port 5000

然后访问:
    http://localhost:5000/          - 查看摄像头信息
    http://localhost:5000/video_stream - 查看视频流
"""
import argparse
import sys
import time


def main():
    parser = argparse.ArgumentParser(description="OpenCV Camera Test Server")
    parser.add_argument("--device", type=int, default=0, help="Camera device index")
    parser.add_argument("--width", type=int, default=320, help="Frame width")
    parser.add_argument("--height", type=int, default=240, help="Frame height")
    parser.add_argument("--fps", type=int, default=30, help="Frames per second")
    parser.add_argument("--host", type=str, default="0.0.0.0", help="Host to bind")
    parser.add_argument("--port", type=int, default=5000, help="Port to bind")
    args = parser.parse_args()

    import cv2
    from flask import Flask, Response, jsonify

    app = Flask(__name__)

    class Camera:
        _instance = None

        def __init__(self, device=0, width=640, height=480, fps=30):
            self._cap = None
            self._device = device
            self._width = width
            self._height = height
            self._fps = fps

        def open(self):
            self._cap = cv2.VideoCapture(self._device)
            if self._cap.isOpened():
                self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, self._width)
                self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self._height)
                self._cap.set(cv2.CAP_PROP_FPS, self._fps)
                return True
            return False

        def read(self):
            if self._cap is None:
                return False, None
            return self._cap.read()

        def release(self):
            if self._cap:
                self._cap.release()
                self._cap = None

        @classmethod
        def get_instance(cls, **kwargs):
            if cls._instance is None:
                cls._instance = cls(**kwargs)
            return cls._instance

    Camera._instance = None
    camera = Camera.get_instance(device=args.device, width=args.width, height=args.height, fps=args.fps)
    if not camera.open():
        print(f"Error: Cannot open camera device {args.device}")
        sys.exit(1)

    frame_count = [0]

    def generate_frames():
        while True:
            ret, frame = camera.read()
            if not ret or frame is None:
                break

            frame_count[0] += 1
            h, w = frame.shape[:2]
            frame_size = frame.nbytes
            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
            ms = int((time.time() % 1) * 1000)
            full_timestamp = f"{timestamp}.{ms:03d}"
            print(f"[Frame #{frame_count[0]}] {w}x{h} | {frame_size} bytes | {full_timestamp}")

            info_text = f"OpenCV | {w}x{h} | {frame_size} bytes | #{frame_count[0]} | {full_timestamp}"
            cv2.putText(frame, info_text, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (0, 255, 0), 2)

            ret, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
            if not ret:
                continue

            yield (b"--frame\r\n"
                   b"Content-Type: image/jpeg\r\n\r\n"
                   + jpeg.tobytes() + b"\r\n")

    @app.route("/")
    def index():
        return jsonify({
            "camera": "opencv",
            "resolution": f"{camera._width}x{camera._height}",
            "actual_resolution": f"{int(camera._cap.get(cv2.CAP_PROP_FRAME_WIDTH))}x{int(camera._cap.get(cv2.CAP_PROP_FRAME_HEIGHT))}",
            "fps": camera._fps,
            "endpoints": {
                "info": "GET /",
                "stream": "GET /video_stream"
            }
        })

    @app.route("/video_stream")
    def video_stream():
        return Response(generate_frames(), mimetype="multipart/x-mixed-replace; boundary=frame")

    print(f"OpenCV Camera Server started on http://{args.host}:{args.port}")
    print(f"Video stream: http://{args.host}:{args.port}/video_stream")
    print(f"Press Ctrl+C to stop")

    try:
        app.run(host=args.host, port=args.port, threaded=True)
    finally:
        camera.release()


if __name__ == "__main__":
    sys.argv = ["test_camera_opencv.py", "--device", "0", "--width", "320", "--height", "240", "--port", "5001"]
    main()