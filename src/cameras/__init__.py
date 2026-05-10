"""摄像头驱动 - OpenCV 实现"""

from .opencv.camera_opencv import Camera as CameraImpl

__all__ = ["Camera"]
Camera = CameraImpl