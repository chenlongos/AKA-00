import os
import subprocess

import cv2
import tornado.ioloop
import tornado.web
import tornado.httpserver
import tornado.wsgi

from app import create_app
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

            await tornado.gen.sleep(0.03)

    @staticmethod
    def _read_and_encode(collector):
        """在 thread pool 中执行 camera read + JPEG 编码"""
        ret, frame = collector._camera.read()
        if not ret or frame is None:
            return None
        ret, jpg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 40])
        if not ret:
            return None
        return jpg.tobytes()


def make_app():
    return tornado.web.Application(
        [
            (r"/api/camera/stream", MJPEGStreamHandler),
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
