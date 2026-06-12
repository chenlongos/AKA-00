#!/usr/bin/env python3
import os
import struct
import subprocess
import time

import cv2
import tornado.ioloop
import tornado.web
import tornado.httpserver
import tornado.websocket
import tornado.wsgi

from app import create_app
from app.services import get_control_service
from src.state import get_state_collector

app = create_app()
wsgi_container = tornado.wsgi.WSGIContainer(app)


class MJPEGStreamHandler(tornado.web.RequestHandler):
    """Tornado 原生异步 handler，不阻塞 IOLoop"""

    async def get(self):
        collector = get_state_collector()
        if collector._camera is None or not hasattr(collector._camera, "_frame_ready"):
            self.set_status(500)
            self.write({"error": "camera not available"})
            return

        fps = int(self.get_argument("fps", "30"))
        interval = 1.0 / max(1, min(fps, 30))

        self.set_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.set_header("Access-Control-Allow-Origin", "*")
        self.set_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.set_header("Pragma", "no-cache")
        self.set_header("Expires", "0")

        io_loop = tornado.ioloop.IOLoop.current()

        while collector._running:
            jpg_bytes = await io_loop.run_in_executor(
                None, self._read_and_encode, collector
            )
            if jpg_bytes is None:
                await tornado.gen.sleep(0.05)
                continue

            try:
                self.write(b"--frame\r\nContent-Type: image/jpeg\r\n\r\n")
                self.write(jpg_bytes)
                self.write(b"\r\n")
                await self.flush()
            except tornado.iostream.StreamClosedError:
                break

            await tornado.gen.sleep(interval)

    @staticmethod
    def _read_and_encode(collector):
        """在 thread pool 中执行 camera read + JPEG 编码"""
        if collector._camera is None:
            return None
        ret, frame = collector._camera.read()
        if not ret or frame is None:
            return None
        # skip frames older than 2s (camera may have stalled)
        frame_age = time.monotonic() - collector._camera._frame_ts
        if frame_age > 2.0:
            return None
        ret, jpg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 40])
        if not ret:
            return None
        return jpg.tobytes()


class ControlWebSocket(tornado.websocket.WebSocketHandler):
    def check_origin(self, origin):
        return True

    def open(self):
        self.set_nodelay(True)
        self._last_left = 0
        self._last_right = 0
        self._status_ticks = 0
        self._status_timer = tornado.ioloop.PeriodicCallback(
            self._push_status, 200
        )
        self._status_timer.start()

    async def on_message(self, message):
        if not isinstance(message, bytes) or len(message) < 3:
            return
        if message[0] != 0xAA:
            return
        x = message[1] if message[1] < 128 else message[1] - 256
        y = message[2] if message[2] < 128 else message[2] - 256
        left = max(-60, min(60, y + x))
        right = max(-60, min(60, y - x))
        await tornado.ioloop.IOLoop.current().run_in_executor(
            None, get_control_service().run_motor, left, right
        )

    def on_close(self):
        self._status_timer.stop()
        tornado.ioloop.IOLoop.current().add_callback(
            self._stop_motor_async
        )

    async def _stop_motor_async(self):
        await tornado.ioloop.IOLoop.current().run_in_executor(
            None, get_control_service().run_motor, 0, 0
        )

    def _push_status(self):
        collector = get_state_collector()
        left = int(collector._status.left_speed * 1000)
        right = int(collector._status.right_speed * 1000)
        self._status_ticks += 1
        # only push if speed changed or every 2s heartbeat
        if (left == self._last_left and right == self._last_right
                and self._status_ticks < 10):
            return
        self._status_ticks = 0
        self._last_left = left
        self._last_right = right
        buf = struct.pack("<Bhh", 0xBB, left, right)
        try:
            self.write_message(buf, binary=True)
        except tornado.websocket.WebSocketClosedError:
            pass


def make_app():
    return tornado.web.Application(
        [
            (r"/api/camera/stream", MJPEGStreamHandler),
            (r"/ws/control", ControlWebSocket),
            (r".*", tornado.web.FallbackHandler, dict(fallback=wsgi_container)),
        ]
    )


def ensure_cert(cert_path, key_path):
    """Generate self-signed certificate if not present."""
    if os.path.exists(cert_path) and os.path.exists(key_path):
        return True
    try:
        subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-keyout",
                key_path,
                "-out",
                cert_path,
                "-days",
                "3650",
                "-nodes",
                "-subj",
                "/CN=AKA-00",
            ],
            check=True,
            capture_output=True,
        )
        print(f"Self-signed cert generated: {cert_path}")
        return True
    except Exception as e:
        print(f"Failed to generate self-signed cert: {e}")
        return False


if __name__ == "__main__":
    tornado_app = make_app()

    # HTTP
    http_port = int(os.getenv("APP_HTTP_PORT", "5000" if os.name == "nt" else "80"))
    tornado.httpserver.HTTPServer(tornado_app).listen(http_port)
    print(f"HTTP listening on port {http_port}")

    # HTTPS
    cert_path = os.getenv("APP_CERT_PATH", "/root/AKA-00/cert.pem")
    key_path = os.getenv("APP_KEY_PATH", "/root/AKA-00/key.pem")
    if ensure_cert(cert_path, key_path):
        https_port = int(os.getenv("APP_HTTPS_PORT", "5443" if os.name == "nt" else "443"))
        tornado.httpserver.HTTPServer(
            tornado_app,
            ssl_options={
                "certfile": cert_path,
                "keyfile": key_path,
            },
        ).listen(https_port)
        print(f"HTTPS listening on port {https_port}")

    tornado.ioloop.IOLoop.current().start()
