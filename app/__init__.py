from flask import Flask, make_response, request


def create_app():
    app = Flask(__name__, static_folder="../static", template_folder="../templates")
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
    from app.routes.base import base_bp
    from app.routes.camera import camera_bp
    from app.routes.demo import demo_bp
    from app.routes.ota import ota_bp
    app.register_blueprint(system_bp)
    app.register_blueprint(motor_bp)
    app.register_blueprint(arm_bp)
    app.register_blueprint(base_bp)
    app.register_blueprint(camera_bp)
    app.register_blueprint(demo_bp)
    app.register_blueprint(ota_bp)
    app.register_blueprint(wifi_bp)  # WiFi 路由注册到根路径
    app.register_blueprint(frontend_bp)

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
