import subprocess

class Camera:
    """ffmpeg 单帧采集相机"""

    def __init__(self, device="/dev/video0", width=640, height=480):
        self.device = device
        self.width = width
        self.height = height
        self.frame_size = width * height * 3  # rgb24

    def read(self) -> bytes | None:
        """获取一帧原始图像数据（RGB），失败返回 None"""
        cmd = [
            "ffmpeg",
            "-loglevel", "quiet",
            "-f", "v4l2",
            "-i", self.device,
            "-vf", f"scale={self.width}:{self.height}",
            "-frames:v", "1",
            "-f", "rawvideo",
            "-pix_fmt", "rgb24",
            "-",
        ]

        try:
            pipe = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            raw = pipe.stdout.read(self.frame_size)
            pipe.wait()

            if not raw or len(raw) < self.frame_size:
                return None
            return raw
        except Exception:
            return None
