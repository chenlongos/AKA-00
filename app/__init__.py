from flask import Flask


def create_app():
    app = Flask(__name__, static_folder="../static", template_folder="../templates")
    from .services import init_control_service
    from .routes.api import api_bp
    from .routes.wifi import wifi_bp
    from .routes.frontend import frontend_bp

    init_control_service(app)
    app.register_blueprint(api_bp, url_prefix="/api")
    app.register_blueprint(wifi_bp)  # WiFi 路由注册到根路径
    app.register_blueprint(frontend_bp)

    return app
