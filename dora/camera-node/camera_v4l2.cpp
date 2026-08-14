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

// ── 环境变量配置（所有平台共享）──

std::pair<int,int> Camera::target_resolution() {
    int w = 640, h = 480;
    const char* env_w = std::getenv("CAMERA_WIDTH");
    const char* env_h = std::getenv("CAMERA_HEIGHT");
    if (env_w) w = std::atoi(env_w);
    if (env_h) h = std::atoi(env_h);
    if (w <= 0)  w = 640;
    if (h <= 0)  h = 480;
    return {w, h};
}

// ── 枚举摄像头支持的所有格式（调试用）──
static void _enumerate_formats(int fd) {
    std::cout << "[camera] Supported formats:" << std::endl;
    v4l2_fmtdesc fmtdesc; CLEAR(fmtdesc);
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        // 枚举该格式下的分辨率
        v4l2_frmsizeenum frmsize; CLEAR(frmsize);
        frmsize.pixel_format = fmtdesc.pixelformat;
        frmsize.index = 0;
        if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                std::cout << "  " << (char)(fmtdesc.pixelformat)
                          << (char)(fmtdesc.pixelformat >> 8)
                          << (char)(fmtdesc.pixelformat >> 16)
                          << (char)(fmtdesc.pixelformat >> 24)
                          << " " << frmsize.discrete.width << "x"
                          << frmsize.discrete.height << std::endl;
            }
        } else {
            std::cout << "  " << (char)(fmtdesc.pixelformat)
                      << (char)(fmtdesc.pixelformat >> 8)
                      << (char)(fmtdesc.pixelformat >> 16)
                      << (char)(fmtdesc.pixelformat >> 24)
                      << " (unknown sizes)" << std::endl;
        }
        fmtdesc.index++;
    }
}

// ── 设置 MJPEG 编码参数（best-effort）──
static void _configure_mjpeg(int fd) {
    // 标准 V4L2 JPEG 质量控制（SG2002 驱动不支持，但不报错）
    v4l2_control ctrl;
    CLEAR(ctrl);
    ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    ctrl.value = 60;
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) == 0)
        std::cout << "[camera] V4L2 JPEG quality set to " << ctrl.value << std::endl;

    // 尝试 MPEG 码率控制（部分 MJPEG 编码器复用此接口）
    v4l2_streamparm parm; CLEAR(parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_PARM, &parm) == 0 &&
        parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = 30;
        ioctl(fd, VIDIOC_S_PARM, &parm);
    }
}

bool Camera::open(const char* device, int target_w, int target_h) {
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

    std::cout << "[camera] V4L2 driver: " << cap.driver
              << " card: " << cap.card << std::endl;

    _enumerate_formats(_fd);

    // ── 1. 优先 MJPEG（web-server 原生直通转发给浏览器；screen-node 解 JPEG）──
    //     YUYV 曾被试过让 screen-node 免解码直写屏，但打破了 web 预览
    //     （web-server 只认 JPEG/RGB）。回退 MJPEG 后两端都能正常工作。
    std::cout << "[camera] trying MJPEG " << target_w << "x" << target_h << "..." << std::endl;
    _width = target_w;
    _height = target_h;
    _fmt = V4L2_PIX_FMT_MJPEG;
    bool mjpeg_ok = _probe_fmt(_fmt, _width, _height);

    std::string format_name = "MJPEG";
    if (!mjpeg_ok) {
        std::cout << "[camera] MJPEG unavailable, falling back to YUYV" << std::endl;
        _width  = target_w;
        _height = target_h;
        _fmt    = V4L2_PIX_FMT_YUYV;
        if (!_probe_fmt(_fmt, _width, _height)) {
            std::fprintf(stderr, "[camera] neither MJPEG nor YUYV supported by this device\n");
            close(); return false;
        }
        format_name = "YUYV";
    }

    if (_fmt == V4L2_PIX_FMT_MJPEG)
        _configure_mjpeg(_fd);

    // ── 4. 初始化缓冲区并启动流 ──
    _init_buffers();
    _start_stream();

    std::cout << "[camera] V4L2 " << _width << "x" << _height
              << " " << format_name
              << " (target was " << target_w << "x"
              << target_h << ")" << std::endl;
    return true;
}

void Camera::close() {
    if (_fd >= 0) {
        _stop_stream();
        ::close(_fd);
        _fd = -1;
    }
    // 兜底：open 失败后 _fd 已置 -1，_stop_stream 被跳过；但 _bufs 可能仍指向
    // 残留 (老 _init_buffers 没清干净)。无脑调一次，安全可重入 (null = no-op)。
    _cleanup_buffers();
    std::cout << "[camera] V4L2 closed" << std::endl;
}

// 仅停流，保留 fd 和设备状态（反复 start/stop 时避免 reopen 开销）
void Camera::stop_stream() {
    if (_fd < 0 || _n_bufs == 0) return;
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(_fd, VIDIOC_STREAMOFF, &type);
}

// 恢复流（重新 QBUF + STREAMON）
void Camera::start_stream() {
    if (_fd < 0 || _n_bufs == 0) return;
    // 重新入队所有 buffer
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

/// 尝试设置像素格式和分辨率，返回实际协商结果
bool Camera::_probe_fmt(uint32_t fmt, int& w, int& h) {
    _fmt = fmt;
    return _try_fmt(w, h);
}

bool Camera::_try_fmt(int& w, int& h) {
    v4l2_format fmt; CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = (unsigned)w;
    fmt.fmt.pix.height      = (unsigned)h;
    fmt.fmt.pix.pixelformat = _fmt;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    if (ioctl(_fd, VIDIOC_S_FMT, &fmt) < 0) return false;
    w = (int)fmt.fmt.pix.width;
    h = (int)fmt.fmt.pix.height;
    return fmt.fmt.pix.pixelformat == _fmt;
}

void Camera::_init_buffers() {
    // 失败路径：之前是 `_bufs = new Buf[N]` + 循环失败只设 `_fd=-1; return`，
    // _bufs 没人 free，close() 又因为 _fd<0 跳过 _stop_stream——每次重试泄漏 64B*N。
    // 现在 _bufs/_n_bufs 全程受 _cleanup_buffers 兜底，错误路径调它清干净。
    _bufs = nullptr;
    _n_bufs = 0;

    v4l2_requestbuffers req; CLEAR(req);
    req.count = 2;  // 减少缓冲数以降低延迟（4→2：~167ms→~83ms pipeline depth）
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(_fd, VIDIOC_REQBUFS, &req) < 0) {
        std::fprintf(stderr, "[camera] V4L2: VIDIOC_REQBUFS: %s\n", std::strerror(errno));
        _fd = -1; return;
    }
    _n_bufs = req.count;
    _bufs = new Buf[_n_bufs];
    std::memset(_bufs, 0, sizeof(Buf) * _n_bufs);   // 0-init: ptr=null, len=0 — 让 _cleanup_buffers 跳过未 mmap 的

    for (unsigned i = 0; i < _n_bufs; i++) {
        v4l2_buffer buf; CLEAR(buf);
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            std::fprintf(stderr, "[camera] V4L2: QUERYBUF[%u]: %s\n", i, std::strerror(errno));
            _cleanup_buffers();
            _fd = -1; return;
        }
        _bufs[i].len = buf.length;
        _bufs[i].ptr = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, buf.m.offset);
        if (_bufs[i].ptr == MAP_FAILED) {
            std::fprintf(stderr, "[camera] V4L2: mmap[%u]: %s\n", i, std::strerror(errno));
            _bufs[i].ptr = nullptr;            // 防止 _cleanup_buffers 把 MAP_FAILED 当合法地址 munmap
            _cleanup_buffers();
            _fd = -1; return;
        }
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
    if (_fd >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(_fd, VIDIOC_STREAMOFF, &type);
    }
    _cleanup_buffers();
}

void Camera::_cleanup_buffers() {
    if (!_bufs) return;                // 已清干净 / 未分配 → no-op；可重复调用
    for (unsigned i = 0; i < _n_bufs; i++) {
        if (_bufs[i].ptr && _bufs[i].ptr != MAP_FAILED)
            munmap(_bufs[i].ptr, _bufs[i].len);
    }
    delete[] _bufs;
    _bufs = nullptr;
    _n_bufs = 0;
}

#endif  // __linux__
