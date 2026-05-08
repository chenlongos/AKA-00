"""FFmpeg MJPEG 流服务器（Linux 专用）。

用法:
    python test_camera_ffmpeg.py

然后访问:
    http://localhost:5002/video_stream - 查看视频流
"""
import subprocess
import threading
import queue


def main():
    width = 224
    height = 224
    port = 5002
    device = "/dev/video0"

    from flask import Flask, Response

    app = Flask(__name__)

    frame_queue = queue.Queue(maxsize=5)
    running = [True]

    # FFmpeg 输出 MJPEG 格式
    cmd = [
        "ffmpeg",
        "-f", "v4l2",
        "-input_format", "mjpeg",
        "-i", device,
        "-vf", f"scale={width}:{height}",
        "-f", "mjpeg",
        "-"
    ]

    def read_ffmpeg():
        try:
            pipe = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        except FileNotFoundError:
            print("Error: ffmpeg not found")
            running[0] = False
            return

        # MJPEG 每帧是独立的 JPEG，用 \xff\xd8 开头，\xff\xd9 结尾
        JPEG_START = b"\xff\xd8"
        JPEG_END = b"\xff\xd9"

        buffer = b""

        while running[0]:
            try:
                chunk = pipe.stdout.read(8192)
                if not chunk:
                    break
                buffer += chunk

                # 提取完整 JPEG 帧
                while True:
                    start = buffer.find(JPEG_START)
                    if start == -1:
                        break
                    end = buffer.find(JPEG_END, start + 2)
                    if end == -1:
                        buffer = buffer[start:]
                        break

                    jpeg = buffer[start:end + 2]
                    buffer = buffer[end + 2:]

                    try:
                        frame_queue.put_nowait(jpeg)
                    except queue.Full:
                        pass  # 丢弃旧帧，保持低延迟
            except Exception:
                break

    reader = threading.Thread(target=read_ffmpeg, daemon=True)
    reader.start()

    def generate():
        while running[0]:
            try:
                jpeg = frame_queue.get(timeout=1)
            except queue.Empty:
                continue

            yield (b"--frame\r\n"
                   b"Content-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n")

    @app.route("/")
    def index():
        return f"""
        <html><body>
        <h1>FFmpeg Camera</h1>
        <p>Resolution: {width}x{height}</p>
        <img src="/video_stream" width="{width}" height="{height}" />
        </body></html>
        """

    @app.route("/video_stream")
    def video_stream():
        return Response(generate(), mimetype="multipart/x-mixed-replace; boundary=frame")

    print(f"FFmpeg Camera Server: http://localhost:{port}/video_stream")
    print(f"Resolution: {width}x{height}")
    try:
        app.run(host="0.0.0.0", port=port, threaded=True)
    finally:
        running[0] = False


if __name__ == "__main__":
    main()
