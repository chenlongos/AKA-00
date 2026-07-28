from flask import Flask, make_response, request


def create_app():
    app = Flask(__name__, static_folder="../static", template_folder="../templates")

    # 如果上次 OTA 安装未完成标记，在服务启动后标记为完成
    try:
        import json, os, time
        status_file = "/root/aka-ota-status.json"
        if os.path.exists(status_file):
            with open(status_file) as f:
                s = json.load(f)
            if s.get("status") in ("downloading", "installing"):
                v, ts = "", "0"
                ver_file = os.path.join(os.path.dirname(__file__), "..", "VERSION")
                if os.path.exists(ver_file):
                    raw = open(ver_file).read().strip()
                    sep = "@" if "@" in raw else " "
                    parts = raw.rsplit(sep, 1) if sep in raw else [raw, raw]
                    v, ts = parts[0], parts[1]
                with open(status_file, "w") as f:
                    json.dump({"status": "completed", "version": v, "updated": int(ts),
                               "finished_at": int(time.time())}, f)
    except Exception:
        pass

    from .services import init_control_service
    from .routes.api import api_bp
    from .routes.wifi import wifi_bp
    from .routes.frontend import frontend_bp

    @app.before_request
    def handle_cors_preflight():
        if request.method == "OPTIONS":
            return _with_cors_headers(make_response("", 204))

    @app.after_request
    def add_cors_headers(response):
        return _with_cors_headers(response)

    init_control_service(app)
    # 先注册 api_bp（包含 /control）
    app.register_blueprint(api_bp)
    from app.routes.system import system_bp
    from app.routes.motor import motor_bp
    from app.routes.arm import arm_bp
    from app.routes.camera import camera_bp
    from app.routes.demo import demo_bp
    from app.routes.ota import ota_bp
    from app.routes.config import config_bp
    app.register_blueprint(system_bp)
    app.register_blueprint(motor_bp)
    app.register_blueprint(arm_bp)
    app.register_blueprint(camera_bp)
    app.register_blueprint(demo_bp)
    app.register_blueprint(ota_bp)
    app.register_blueprint(config_bp)
    app.register_blueprint(wifi_bp)  # WiFi 路由注册到根路径
    app.register_blueprint(frontend_bp)

    # 启动状态上报（云端）
    from app.services.status_reporter import start as start_reporter
    start_reporter()

    # 启动状态采集线程
    from src.state import get_state_collector
    collector = get_state_collector()
    collector.set_motor_pair(app.extensions["control_service"]._motor_pair)
    collector.start()

    return app


def _with_cors_headers(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Methods"] = "GET,POST,PUT,PATCH,DELETE,OPTIONS"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type,Authorization"
    response.headers["Access-Control-Max-Age"] = "86400"
    return response
