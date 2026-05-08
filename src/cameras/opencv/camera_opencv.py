"""OpenCV 摄像头驱动"""
import threading

try:
    import cv2
    _HAS_CV2 = True
except ImportError:
    _HAS_CV2 = False


class Camera:
    """
    OpenCV 连续视频流摄像头。

    使用单例模式，整个进程共享一个 VideoCapture 实例。
    """

    _instance: "Camera | None" = None
    _lock = threading.Lock()

    def __init__(self, device: int = 0, width: int = 160, height: int = 120, fps: int = 10):
        self._cap = None
        self._cap_lock = threading.Lock()
        self._device = device
        self._width = width
        self._height = height
        self._fps = fps

    @classmethod
    def get_instance(cls, device: int = 0, width: int = 160, height: int = 120, fps: int = 10) -> "Camera":
        """获取 Camera 单例"""
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = Camera(device, width, height, fps)
                    cls._instance._open()
        return cls._instance

    def _open(self) -> bool:
        """打开摄像头"""
        if not _HAS_CV2:
            return False
        with self._cap_lock:
            if self._cap is None:
                self._cap = cv2.VideoCapture(self._device)
                if self._cap.isOpened():
                    self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, self._width)
                    self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self._height)
                    self._cap.set(cv2.CAP_PROP_FPS, self._fps)
                    return True
                self._cap = None
                return False
            return True

    def is_available(self) -> bool:
        """检查摄像头是否可用"""
        with self._cap_lock:
            return self._cap is not None and self._cap.isOpened()

    def read(self):
        """
        读取一帧图像。

        Returns:
            tuple: (success: bool, frame: ndarray or None)
        """
        with self._cap_lock:
            if self._cap is None or not self._cap.isOpened():
                return False, None
            ret, frame = self._cap.read()
            return ret, frame

    def release(self):
        """释放摄像头资源"""
        with self._cap_lock:
            if self._cap is not None:
                self._cap.release()
                self._cap = None

    @classmethod
    def reset(cls):
        """重置单例（用于重新初始化）"""
        with cls._lock:
            if cls._instance is not None:
                cls._instance.release()
            cls._instance = None
