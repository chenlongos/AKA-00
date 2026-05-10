"""V4L2 摄像头驱动 - 直接调用 Linux 内核 API，绕过 OpenCV"""
import threading
import time

try:
    from v4l2py import Video, device
    _HAS_V4L2 = True
except ImportError:
    _HAS_V4L2 = False


class Camera:
    """
    V4L2 摄像头驱动。

    使用单例模式 + 独立采集线程，只保留最新帧。
    与 OpenCV Camera 接口兼容，可互换。
    """

    _instance: "Camera | None" = None
    _lock = threading.Lock()

    def __init__(self, device: int = 0, width: int = 320, height: int = 180, fps: int = 10):
        self._video: Video | None = None
        self._device_id = device
        self._width = width
        self._height = height
        self._fps = fps
        self._frame = None
        self._frame_lock = threading.Lock()
        self._frame_ready = threading.Event()
        self._running = False
        self._thread: threading.Thread | None = None

    @classmethod
    def get_instance(cls, device: int = 0, width: int = 320, height: int = 180, fps: int = 10) -> "Camera":
        """获取 Camera 单例"""
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = Camera(device, width, height, fps)
                    if not cls._instance._open():
                        raise RuntimeError(f"Failed to open V4L2 device {device}")
                    cls._instance._start_capture()
        return cls._instance

    def _open(self) -> bool:
        """打开摄像头"""
        if not _HAS_V4L2:
            print("[Camera] v4l2py not available")
            return False

        try:
            self._video = Video(self._device_id)
            # 设置 MJPG 格式（摄像头硬件解码）
            self._video.set_format(self._width, self._height, 'MJPG')
            self._video.set_fps(self._fps)

            fmt = self._video.get_format()
            print(f"[Camera] V4L2 Opened: {fmt.width}x{fmt.height} @ {self._fps}fps, fourcc={fmt.fourcc}")
            return True
        except Exception as e:
            print(f"[Camera] Failed to open V4L2 device {self._device_id}: {e}")
            self._video = None
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
            if self._video is None:
                time.sleep(0.1)
                continue
            try:
                # capture() 是阻塞的，超时返回 None
                frame = self._video.capture(timeout=1.0)
                if frame is None:
                    time.sleep(0.01)
                    continue
                # v4l2py 返回的 frame 是 numpy array，已经解码
                with self._frame_lock:
                    self._frame = frame
                self._frame_ready.set()
            except Exception:
                time.sleep(0.01)
                continue

    def is_available(self) -> bool:
        """检查摄像头是否可用"""
        return self._video is not None

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
        if self._video is not None:
            try:
                self._video.close()
            except Exception:
                pass
            self._video = None

    @classmethod
    def reset(cls):
        """重置单例"""
        with cls._lock:
            if cls._instance is not None:
                cls._instance.release()
            cls._instance = None