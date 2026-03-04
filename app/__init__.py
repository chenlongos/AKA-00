from flask import Flask

def create_app():
    app = Flask(__name__, static_folder="../static", template_folder="../templates")
    from .routes.api import api_bp
    from .routes.frontend import frontend_bp
    from .extensions import socketio

    app.register_blueprint(api_bp, url_prefix="/api")
    app.register_blueprint(frontend_bp)

    socketio.init_app(app, cors_allowed_origins="*", async_mode="threading")

    # 仅在需要时导入模拟相关的websocket路由
    import os
    if os.getenv("ENABLE_SIMULATOR", "false").lower() == "true" or os.name == "nt":
        from .routes import websocket

    return app