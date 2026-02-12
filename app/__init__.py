from flask import Flask



def create_app():
    app = Flask(__name__, static_folder="../static", template_folder="../templates")
    from .routes.api import api_bp
    from .routes.frontend import frontend_bp

    app.register_blueprint(api_bp, url_prefix="/api")
    app.register_blueprint(frontend_bp)

    return app

