"""测试 V4L2 摄像头视频流，带分辨率和帧大小标记。

用法:
    python test_camera_v4l2.py --device 0 --width 320 --height 180 --port 5001

然后访问:
    http://localhost:5001/          - 查看摄像头信息
    http://localhost:5001/video_stream - 查看视频流
"""
import argparse
import sys
import time


def main():
    parser = argparse.ArgumentParser(description="V4L2 Camera Test Server")
    parser.add_argument("--device", type=int, default=0, help="Camera device index")
    parser.add_argument("--width", type=int, default=320, help="Frame width")
    parser.add_argument("--height", type=int, default=180, help="Frame height")
    parser.add_argument("--fps", type=int, default=10, help="Frames per second")
    parser.add_argument("--host", type=str, default="0.0.0.0", help="Host to bind")
    parser.add_argument("--port", type=int, default=5001, help="Port to bind")
    args = parser.parse_args()

    try:
        from v4l2py import Video, device
    except ImportError:
        print("Error: v4l2py not installed. Install with: pip install v4l2py")
        sys.exit(1)

    from flask import Flask, Response, jsonify

    app = Flask(__name__)

    class Camera:
        """V4L2 摄像头驱动，单例模式 + 独立采集线程，只保留最新帧。"""

        _instance = None
        _lock = __import__("threading").Lock()

        def __init__(self, device: int = 0, width: int = 320, height: int = 180, fps: int = 10):
            self._video: Video | None = None
            self._device_id = device
            self._width = width
            self._height = height
            self._fps = fps
            self._frame = None
            self._frame_lock = __import__("threading").Lock()
            self._frame_ready = __import__("threading").Event()
            self._running = False
            self._thread: __import__("threading").Thread | None = None

        def _open(self) -> bool:
            """打开摄像头"""
            try:
                self._video = Video(self._device_id)
                # 设置 MJPG 格式（摄像头硬件解码）
                self._video.set_format(self._width, self._height, "MJPG")
                self._video.set_fps(self._fps)
                fmt = self._video.get_format()
                print(f"[Camera] V4L2 Opened: {fmt.width}x{fmt.height} @ {self._fps}fps, fourcc={fmt.fourcc}")
                return True
            except Exception as e:
                print(f"[Camera] Failed to open V4L2 device {self._device_id}: {e}")
                self._video = None
                return False

        def _start_capture(self):
            """启动采集线程"""
            if self._running:
                return
            self._running = True
            self._thread = __import__("threading").Thread(target=self._capture_loop, daemon=True)
            self._thread.start()

        def _capture_loop(self):
            """独立采集线程，只保留最新帧"""
            while self._running:
                if self._video is None:
                    time.sleep(0.1)
                    continue
                try:
                    frame = self._video.capture(timeout=1.0)
                    if frame is None:
                        time.sleep(0.01)
                        continue
                    with self._frame_lock:
                        self._frame = frame
                    self._frame_ready.set()
                except Exception:
                    time.sleep(0.01)
                    continue

        def is_available(self) -> bool:
            """检查摄像头是否可用"""
            return self._video is not None

        def read(self):
            """读取最新帧（不拷贝，直接返回引用）"""
            with self._frame_lock:
                if self._frame is None:
                    return False, None
                return True, self._frame

        def release(self):
            """释放摄像头资源"""
            self._running = False
            if self._thread is not None:
                self._thread.join(timeout=1)
                self._thread = None
            with self._frame_lock:
                self._frame = None
            if self._video is not None:
                try:
                    self._video.close()
                except Exception:
                    pass
                self._video = None

        @classmethod
        def get_instance(cls, device: int = 0, width: int = 320, height: int = 180, fps: int = 10) -> "Camera":
            """获取 Camera 单例"""
            if cls._instance is None:
                with cls._lock:
                    if cls._instance is None:
                        cls._instance = Camera(device, width, height, fps)
                        if not cls._instance._open():
                            raise RuntimeError(f"Failed to open V4L2 device {device}")
                        cls._instance._start_capture()
            return cls._instance

        @classmethod
        def reset(cls):
            """重置单例"""
            with cls._lock:
                if cls._instance is not None:
                    cls._instance.release()
                cls._instance = None

    Camera._instance = None
    camera = Camera.get_instance(device=args.device, width=args.width, height=args.height, fps=args.fps)

    if not camera.is_available():
        print(f"Error: Cannot open V4L2 device {args.device}")
        sys.exit(1)

    frame_count = [0]

    def generate_frames():
        while True:
            ret, frame = camera.read()
            if not ret or frame is None:
                continue

            frame_count[0] += 1
            h, w = frame.shape[:2]
            frame_size = frame.nbytes

            info_text = f"V4L2 | {w}x{h} | {frame_size} bytes | #{frame_count[0]}"
            import cv2
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
            "camera": "v4l2",
            "resolution": f"{camera._width}x{camera._height}",
            "fps": camera._fps,
            "endpoints": {
                "info": "GET /",
                "stream": "GET /video_stream"
            }
        })

    @app.route("/video_stream")
    def video_stream():
        return Response(generate_frames(), mimetype="multipart/x-mixed-replace; boundary=frame")

    print(f"V4L2 Camera Server started on http://{args.host}:{args.port}")
    print(f"Video stream: http://{args.host}:{args.port}/video_stream")
    print(f"Press Ctrl+C to stop")

    try:
        app.run(host=args.host, port=args.port, threaded=True)
    finally:
        camera.release()


if __name__ == "__main__":
    sys.argv = ["test_camera_v4l2.py", "--device", "0", "--width", "320", "--height", "180", "--port", "5001"]
    main()