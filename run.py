import os
import subprocess
import tornado.ioloop
import tornado.httpserver
import tornado.wsgi

from app import create_app

app = create_app()
container = tornado.wsgi.WSGIContainer(app)


def ensure_cert(cert_path, key_path):
    """Generate self-signed certificate if not present."""
    if os.path.exists(cert_path) and os.path.exists(key_path):
        return True
    try:
        subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048",
                "-keyout", key_path, "-out", cert_path,
                "-days", "3650", "-nodes",
                "-subj", "/CN=AKA-00",
            ],
            check=True, capture_output=True,
        )
        print(f"Self-signed cert generated: {cert_path}")
        return True
    except Exception as e:
        print(f"Failed to generate self-signed cert: {e}")
        return False


if __name__ == '__main__':
    # HTTP
    http_port = int(os.getenv("APP_HTTP_PORT", "5000" if os.name == "nt" else "80"))
    tornado.httpserver.HTTPServer(container).listen(http_port)
    print(f"HTTP listening on port {http_port}")

    # HTTPS
    cert_path = os.getenv("APP_CERT_PATH", "/root/AKA-00/cert.pem")
    key_path = os.getenv("APP_KEY_PATH", "/root/AKA-00/key.pem")
    if ensure_cert(cert_path, key_path):
        https_port = int(os.getenv("APP_HTTPS_PORT", "5443" if os.name == "nt" else "443"))
        tornado.httpserver.HTTPServer(container, ssl_options={
            "certfile": cert_path,
            "keyfile": key_path,
        }).listen(https_port)
        print(f"HTTPS listening on port {https_port}")

    tornado.ioloop.IOLoop.current().start()
