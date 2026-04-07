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
    app.register_blueprint(api_bp, url_prefix="/api")
    app.register_blueprint(wifi_bp)  # WiFi 路由注册到根路径
    app.register_blueprint(frontend_bp)

    return app


def _with_cors_headers(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Methods"] = "GET,POST,PUT,PATCH,DELETE,OPTIONS"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type,Authorization"
    response.headers["Access-Control-Max-Age"] = "86400"
    return response
