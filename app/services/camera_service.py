"""摄像头服务单例 - 简化版，直接使用 Camera"""
import threading


class CameraService:
    """全局摄像头单例，直接透传给 Camera"""

    _instance: "CameraService | None" = None
    _lock = threading.Lock()

    def __init__(self):
        self._camera = None

    @classmethod
    def get_instance(cls) -> "CameraService":
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = CameraService()
        return cls._instance

    def _ensure_camera(self):
        """确保摄像头已初始化"""
        if self._camera is None:
            from src.cameras.opencv import Camera as OpenCVCamera
            self._camera = OpenCVCamera.get_instance(
                device=0,
                width=320,
                height=180,
                fps=15
            )
            from src.state import get_state_collector
            get_state_collector().set_camera(self._camera)

    def is_available(self) -> bool:
        return self._camera is not None and self._camera.is_available()

    def read(self):
        """读取最新帧（直接透传，不自己读）"""
        return self._camera.read() if self._camera else (False, None)

    def close(self):
        """关闭摄像头"""
        if self._camera is not None:
            from src.cameras.opencv import Camera as OpenCVCamera
            OpenCVCamera.reset()
            from src.state import get_state_collector
            get_state_collector().clear_camera()
            self._camera = None
