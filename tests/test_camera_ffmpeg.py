"""测试 FFmpeg 摄像头视频流，带分辨率和帧大小标记（Linux 专用）。

用法:
    python test_camera_ffmpeg.py
"""
import subprocess
import sys
import time
import threading
import Queue


def main():
    device = "/dev/video0"
    width = 640
    height = 480
    port = 5002

    import cv2
    import numpy as np
    from flask import Flask, Response, jsonify

    app = Flask(__name__)

    frame_size = width * height * 3  # rgb24
    frame_queue = Queue.Queue(maxsize=2)
    running = [True]
    frame_count = [0]
    last_frame_time = [time.time()]

    cmd = [
        "ffmpeg",
        "-loglevel", "quiet",
        "-f", "v4l2",
        "-input_format", "mjpeg",
        "-thread_queue_size", "64",
        "-i", device,
        "-vf", f"scale={width}:{height}",
        "-f", "rawvideo",
        "-pix_fmt", "rgb24",
        "-"
    ]

    try:
        pipe = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    except FileNotFoundError:
        print("Error: ffmpeg not found. Please install ffmpeg on Linux.")
        sys.exit(1)

    def read_frames():
        while running[0]:
            try:
                raw = b""
                while len(raw) < frame_size:
                    chunk = pipe.stdout.read(frame_size - len(raw))
                    if not chunk:
                        time.sleep(0.01)
                        continue
                    raw += chunk

                if len(raw) == frame_size:
                    frame = np.frombuffer(raw, dtype=np.uint8).reshape(height, width, 3)
                    frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
                    try:
                        frame_queue.put_nowait(frame)
                    except Queue.Full:
                        pass
            except Exception as e:
                print(f"Read error: {e}")
                time.sleep(0.1)

    reader_thread = threading.Thread(target=read_frames, daemon=True)
    reader_thread.start()

    def generate_frames():
        while running[0]:
            try:
                frame = frame_queue.get(timeout=1)
            except Queue.Empty:
                continue

            frame_count[0] += 1
            now = time.time()
            delta_ms = (now - last_frame_time[0]) * 1000
            last_frame_time[0] = now

            frame_size_bytes = frame.nbytes

            info_text = f"FFmpeg | {width}x{height} | {frame_size_bytes}B | dt:{delta_ms:.0f}ms | #{frame_count[0]}"
            cv2.putText(frame, info_text, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)

            ret, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
            if not ret:
                continue

            yield (b"--frame\r\n"
                   b"Content-Type: image/jpeg\r\n\r\n"
                   + jpeg.tobytes() + b"\r\n")

    @app.route("/")
    def index():
        return jsonify({
            "camera": "ffmpeg",
            "resolution": f"{width}x{height}",
            "frame_size": frame_size,
            "device": device,
            "stream_url": "/video_stream"
        })

    @app.route("/video_stream")
    def video_stream():
        return Response(generate_frames(), mimetype="multipart/x-mixed-replace; boundary=frame")

    print(f"FFmpeg Camera Server started on http://0.0.0.0:{port}")
    print(f"Video stream: http://0.0.0.0:{port}/video_stream")

    try:
        app.run(host="0.0.0.0", port=port, threaded=True)
    finally:
        running[0] = False
        pipe.terminate()
        pipe.wait()


if __name__ == "__main__":
    main()
