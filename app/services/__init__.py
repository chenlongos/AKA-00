from flask import current_app

from app.config import config
from .control_service import ControlService


def init_control_service(app) -> None:
    app.extensions["control_service"] = ControlService(config)


def get_control_service() -> ControlService:
    return current_app.extensions["control_service"]


__all__ = ["ControlService", "get_control_service", "init_control_service"]
