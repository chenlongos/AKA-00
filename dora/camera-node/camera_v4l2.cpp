// camera_v4l2.cpp — Linux V4L2 摄像头实现
// 编译: 仅 Linux (g++ -std=c++17)
#ifdef __linux__

#include "camera.h"

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstring>
#include <iostream>

#define CLEAR(x) std::memset(&(x), 0, sizeof(x))

Camera::Camera()  = default;
Camera::~Camera() { close(); }

bool Camera::open(const char* device) {
    if (!device) device = "/dev/video0";
    _fd = ::open(device, O_RDWR | O_NONBLOCK);
    if (_fd < 0) {
        std::fprintf(stderr, "[camera] V4L2: cannot open %s: %s\n", device, std::strerror(errno));
        return false;
    }

    v4l2_capability cap; CLEAR(cap);
    if (ioctl(_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        std::fprintf(stderr, "[camera] V4L2: VIDIOC_QUERYCAP: %s\n", std::strerror(errno));
        close(); return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
        std::fprintf(stderr, "[camera] V4L2: %s not a streaming capture device\n", device);
        close(); return false;
    }

    // 优先 MJPEG → YUYV
    _fmt = V4L2_PIX_FMT_MJPEG;
    _try_fmt(_width, _height);
    if (_width != 640 || _height != 480) {
        std::fprintf(stderr, "[camera] V4L2: MJPEG gives %dx%d, retry YUYV...\n", _width, _height);
        _fmt = V4L2_PIX_FMT_YUYV;
        _width = 640; _height = 480;
        _try_fmt(_width, _height);
    }

    // 降低 MJPEG 压缩质量以减小帧大小（SG2002 硬件编码器默认质量偏高）
    if (_fmt == V4L2_PIX_FMT_MJPEG) {
        v4l2_control ctrl;
        CLEAR(ctrl);
        ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
        ctrl.value = 50;  // 0-100，默认 80-90 → 50 可把 75KB 压到 ~35KB
        if (ioctl(_fd, VIDIOC_S_CTRL, &ctrl) == 0)
            std::cout << "[camera] V4L2 JPEG quality: " << ctrl.value << std::endl;
    }

    _init_buffers();
    _start_stream();

    const char* name = (_fmt == V4L2_PIX_FMT_MJPEG) ? "MJPEG" : "YUYV";
    std::cout << "[camera] V4L2 " << _width << "x" << _height << " " << name << std::endl;
    return true;
}

void Camera::close() {
    if (_fd >= 0) {
        _stop_stream();
        ::close(_fd);
        _fd = -1;
    }
    std::cout << "[camera] V4L2 closed" << std::endl;
}

bool Camera::good()    const { return _fd >= 0; }
int  Camera::fd()      const { return _fd; }
int  Camera::width()   const { return _width; }
int  Camera::height()  const { return _height; }
bool Camera::is_mjpeg() const { return _fmt == V4L2_PIX_FMT_MJPEG; }

bool Camera::wait_frame(int timeout_ms) {
    struct pollfd pfd = {_fd, POLLIN, 0};
    return poll(&pfd, 1, timeout_ms) > 0;
}

std::pair<const uint8_t*, size_t> Camera::read_frame() {
    v4l2_buffer buf; CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(_fd, VIDIOC_DQBUF, &buf) < 0) return {nullptr, 0};

    auto* src = static_cast<const uint8_t*>(_bufs[buf.index].ptr);
    size_t len = buf.bytesused;
    _frame.assign(src, src + len);
    ioctl(_fd, VIDIOC_QBUF, &buf);
    return {_frame.data(), _frame.size()};
}

// ── private ──

bool Camera::_try_fmt(int& w, int& h) {
    v4l2_format fmt; CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = w;
    fmt.fmt.pix.height      = h;
    fmt.fmt.pix.pixelformat = _fmt;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    if (ioctl(_fd, VIDIOC_S_FMT, &fmt) < 0) return false;
    w = fmt.fmt.pix.width;
    h = fmt.fmt.pix.height;
    return fmt.fmt.pix.pixelformat == _fmt;
}

void Camera::_init_buffers() {
    v4l2_requestbuffers req; CLEAR(req);
    req.count = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(_fd, VIDIOC_REQBUFS, &req) < 0) {
        std::fprintf(stderr, "[camera] V4L2: VIDIOC_REQBUFS: %s\n", std::strerror(errno));
        _fd = -1; return;
    }
    _n_bufs = req.count;
    _bufs = new Buf[_n_bufs];

    for (unsigned i = 0; i < _n_bufs; i++) {
        v4l2_buffer buf; CLEAR(buf);
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(_fd, VIDIOC_QUERYBUF, &buf) < 0) { _fd = -1; return; }
        _bufs[i].len = buf.length;
        _bufs[i].ptr = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, buf.m.offset);
        if (_bufs[i].ptr == MAP_FAILED) { _fd = -1; return; }
    }
}

void Camera::_start_stream() {
    for (unsigned i = 0; i < _n_bufs; i++) {
        v4l2_buffer buf; CLEAR(buf);
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        ioctl(_fd, VIDIOC_QBUF, &buf);
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(_fd, VIDIOC_STREAMON, &type);
}

void Camera::_stop_stream() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(_fd, VIDIOC_STREAMOFF, &type);
    for (unsigned i = 0; i < _n_bufs; i++)
        if (_bufs[i].ptr && _bufs[i].ptr != MAP_FAILED)
            munmap(_bufs[i].ptr, _bufs[i].len);
    delete[] _bufs;
    _bufs = nullptr;
    _n_bufs = 0;
}

#endif  // __linux__
