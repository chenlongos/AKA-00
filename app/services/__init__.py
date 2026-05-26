from app.config import config
from .control_service import ControlService

_control_service: ControlService | None = None


def init_control_service(app) -> None:
    service = ControlService(config)
    app.extensions["control_service"] = service
    global _control_service
    _control_service = service


def set_control_service(service: ControlService) -> None:
    global _control_service
    _control_service = service


def get_control_service() -> ControlService:
    if _control_service is None:
        raise RuntimeError("ControlService not initialized")
    return _control_service


__all__ = ["ControlService", "get_control_service", "set_control_service", "init_control_service"]
