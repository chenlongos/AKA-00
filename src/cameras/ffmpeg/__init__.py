import subprocess
import time

width = 640
height = 480

cmd = [
    "ffmpeg",
    "-loglevel", "quiet",   # 👈 关键：关闭所有输出
    "-f", "v4l2",
    "-i", "/dev/video0",
    "-vf", "scale=640:480",
    "-r", "15",
    "-f", "rawvideo",
    "-pix_fmt", "rgb24",
    "-"
]

pipe = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=10**8)

frame_size = width * height * 3

count = 0
start = time.time()

while True:
    raw = pipe.stdout.read(frame_size)
    if not raw:
        break

    count += 1

    if time.time() - start >= 1:
        print("FPS:", count)
        count = 0
        start = time.time()