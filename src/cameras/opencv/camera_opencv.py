"""OpenCV 摄像头驱动"""
import threading
import time

try:
    import cv2
    _HAS_CV2 = True
except ImportError:
    _HAS_CV2 = False


class Camera:
    """
    OpenCV 摄像头驱动。

    使用单例模式 + 独立采集线程，只保留最新帧。
    """

    _instance: "Camera | None" = None
    _lock = threading.Lock()

    def __init__(self, device: int = 0, width: int = 320, height: int = 180, fps: int = 15):
        self._cap = None
        self._device = device
        self._width = width
        self._height = height
        self._fps = fps
        self._frame = None
        self._frame_lock = threading.Lock()
        self._running = False
        self._thread: threading.Thread | None = None

    @classmethod
    def get_instance(cls, device: int = 0, width: int = 320, height: int = 180, fps: int = 15) -> "Camera":
        """获取 Camera 单例"""
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = Camera(device, width, height, fps)
                    if not cls._instance._open():
                        raise RuntimeError(f"Failed to open camera device {device}")
                    cls._instance._start_capture()
        return cls._instance

    def _open(self) -> bool:
        """打开摄像头"""
        if not _HAS_CV2:
            print("[Camera] OpenCV not available")
            return False
        self._cap = cv2.VideoCapture(self._device)
        if self._cap.isOpened():
            self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, self._width)
            self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self._height)
            self._cap.set(cv2.CAP_PROP_FPS, self._fps)
            w = int(self._cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            h = int(self._cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            print(f"[Camera] Opened: {w}x{h} @ {self._fps}fps")
            return True
        print(f"[Camera] Failed to open device {self._device}")
        self._cap = None
        return False

    def _start_capture(self):
        """启动采集线程"""
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._capture_loop, daemon=True)
        self._thread.start()

    def _capture_loop(self):
        """独立采集线程，只保留最新帧"""
        while self._running:
            if self._cap is None or not self._cap.isOpened():
                time.sleep(0.1)
                continue
            ret, frame = self._cap.read()
            if not ret:
                time.sleep(0.01)
                continue
            with self._frame_lock:
                self._frame = frame

    def is_available(self) -> bool:
        """检查摄像头是否可用"""
        return self._cap is not None and self._cap.isOpened()

    def read(self):
        """读取最新帧（不拷贝，直接返回引用）"""
        with self._frame_lock:
            if self._frame is None:
                return False, None
            return True, self._frame

    def release(self):
        """释放摄像头资源"""
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=1)
            self._thread = None
        with self._frame_lock:
            self._frame = None
        if self._cap is not None:
            self._cap.release()
            self._cap = None

    @classmethod
    def reset(cls):
        """重置单例"""
        with cls._lock:
            if cls._instance is not None:
                cls._instance.release()
            cls._instance = None
