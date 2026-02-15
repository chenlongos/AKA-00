from flask import Blueprint, render_template

frontend_bp = Blueprint("frontend", __name__)

@frontend_bp.route("/", defaults={"path": ""})
@frontend_bp.route("/<path:path>")
def serve_react(path):
    if path.startswith("api"):
        return {"error": "Not Found"}, 404
    return render_template("index.html")
