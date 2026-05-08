"""摄像头服务单例 - 共享帧缓存"""
import threading
import time

try:
    from src.cameras.opencv import Camera as OpenCVCamera
    _HAS_CV2 = True
except ImportError:
    _HAS_CV2 = False


class CameraService:
    """全局摄像头单例，支持多客户端共享帧缓存"""

    _instance: "CameraService | None" = None
    _lock = threading.Lock()

    def __init__(self):
        self._camera = None
        self._frame = None
        self._frame_lock = threading.Lock()
        self._running = False
        self._thread: threading.Thread | None = None

    @classmethod
    def get_instance(cls) -> "CameraService":
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = CameraService()
        return cls._instance

    def _ensure_camera(self):
        """确保摄像头已初始化"""
        if self._camera is None and _HAS_CV2:
            self._camera = OpenCVCamera.get_instance()

    def _start_capture(self):
        """启动后台采集线程"""
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._capture_loop, daemon=True)
        self._thread.start()

    def _capture_loop(self):
        """后台持续采集帧"""
        while self._running:
            self._ensure_camera()
            if self._camera is not None:
                ret, frame = self._camera.read()
                if ret:
                    with self._frame_lock:
                        self._frame = frame
            time.sleep(0.03)  # ~30fps

    def is_available(self) -> bool:
        self._ensure_camera()
        self._start_capture()
        return self._camera is not None and self._camera.is_available()

    def read(self):
        """读取最新帧（从共享缓存）"""
        self._start_capture()
        with self._frame_lock:
            if self._frame is None:
                return False, None
            return True, self._frame.copy()

    def close(self):
        """关闭摄像头"""
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=1)
            self._thread = None
        if self._camera is not None:
            OpenCVCamera.reset()
            self._camera = None
        with self._frame_lock:
            self._frame = None
