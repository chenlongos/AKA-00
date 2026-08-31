// csrc/camera.cpp — V4L2 + libjpeg 实现（参考 demo_camera.c，C++ 封装）

#include "csrc/camera.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "csrc/log.hpp"

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include <jpeglib.h>
#include <setjmp.h>

namespace {

struct CamBuf {
    void* ptr = nullptr;
    size_t len = 0;
};

// libjpeg 错误桩：setjmp/longjmp 跳出解码/编码
struct JpegErr {
    jpeg_error_mgr pub;
    jmp_buf jump;
};
void jpeg_on_error(j_common_ptr cinfo) {
    char buf[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buf);
    CAM_WARN("JPEG error: %s", buf);
    longjmp(((JpegErr*)cinfo->err)->jump, 1);
}

}  // namespace

namespace csrc {

Camera::~Camera() { close(); }

bool Camera::open_device(int width, int height, int fps) {
    const char* device = "/dev/video0";
    fd_ = ::open(device, O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        CAM_ERROR("V4L2: open %s failed: %s", device, std::strerror(errno));
        return false;
    }
    v4l2_capability cap;
    std::memset(&cap, 0, sizeof cap);
    if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        CAM_ERROR("V4L2: %s not a streaming capture device", device);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    char drv[17] = {0}, crd[33] = {0};
    std::memcpy(drv, cap.driver, 16);
    std::memcpy(crd, cap.card, 32);
    CAM_INFO("V4L2 driver=%s card=%s", drv, crd);

    // 协商格式：MJPEG 优先，YUYV 回退
    auto try_fmt = [&](uint32_t fmt, int& w, int& h) {
        v4l2_format f;
        std::memset(&f, 0, sizeof f);
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        f.fmt.pix.width = (unsigned)w;
        f.fmt.pix.height = (unsigned)h;
        f.fmt.pix.pixelformat = fmt;
        f.fmt.pix.field = V4L2_FIELD_ANY;
        if (::ioctl(fd_, VIDIOC_S_FMT, &f) < 0) return false;
        w = (int)f.fmt.pix.width;
        h = (int)f.fmt.pix.height;
        return f.fmt.pix.pixelformat == fmt;
    };

    int w = width, h = height;
    fmt_ = V4L2_PIX_FMT_MJPEG;
    if (!try_fmt(fmt_, w, h)) {
        w = width; h = height;
        fmt_ = V4L2_PIX_FMT_YUYV;
        if (!try_fmt(fmt_, w, h)) {
            CAM_ERROR("V4L2: neither MJPEG nor YUYV supported");
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }
    CAM_INFO("V4L2 negotiated %s %dx%d",
             fmt_ == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV", w, h);
    cam_w_ = w;
    cam_h_ = h;

    // 请求 30fps（best-effort）
    v4l2_streamparm parm;
    std::memset(&parm, 0, sizeof parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (::ioctl(fd_, VIDIOC_G_PARM, &parm) == 0 &&
        (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = (unsigned)(fps > 0 ? fps : 30);
        ::ioctl(fd_, VIDIOC_S_PARM, &parm);
    }

    // 请求 mmap 双缓冲
    v4l2_requestbuffers req;
    std::memset(&req, 0, sizeof req);
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (::ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        CAM_ERROR("V4L2: REQBUFS failed: %s", std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    nbufs_ = req.count;
    bufs_ = new CamBuf[nbufs_];
    CamBuf* bufs = (CamBuf*)bufs_;
    for (unsigned i = 0; i < nbufs_; i++) {
        v4l2_buffer b;
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (::ioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) {
            CAM_ERROR("V4L2: QUERYBUF[%u]: %s", i, std::strerror(errno));
            return false;
        }
        bufs[i].len = b.length;
        bufs[i].ptr = ::mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
        if (bufs[i].ptr == MAP_FAILED) {
            bufs[i].ptr = nullptr;
            CAM_ERROR("V4L2: mmap[%u]: %s", i, std::strerror(errno));
            return false;
        }
    }
    for (unsigned i = 0; i < nbufs_; i++) {
        v4l2_buffer b;
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        ::ioctl(fd_, VIDIOC_QBUF, &b);
    }
    v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (::ioctl(fd_, VIDIOC_STREAMON, &t) < 0) {
        CAM_ERROR("V4L2: STREAMON failed: %s", std::strerror(errno));
        return false;
    }
    return true;
}

void Camera::close() {
    if (thread_) {
        running_ = false;
        thread_->join();
        delete thread_;
        thread_ = nullptr;
    }
    if (fd_ >= 0) {
        v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ::ioctl(fd_, VIDIOC_STREAMOFF, &t);
        ::close(fd_);
        fd_ = -1;
    }
    if (bufs_) {
        CamBuf* bufs = (CamBuf*)bufs_;
        for (unsigned i = 0; i < nbufs_; i++) {
            if (bufs[i].ptr) ::munmap(bufs[i].ptr, bufs[i].len);
        }
        delete[] bufs;
        bufs_ = nullptr;
        nbufs_ = 0;
    }
    available_ = false;
}

bool Camera::open(int width, int height, int fps) {
    if (available_) return true;
    if (!open_device(width, height, fps)) return false;
    available_ = true;
    running_ = true;
    thread_ = new std::thread([this] { capture_loop(); });
    CAM_INFO("camera capture thread started (%dx%d)", width, height);
    return true;
}

void Camera::capture_loop() {
    while (running_) {
        if (fd_ < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        pollfd pfd = {fd_, POLLIN, 0};
        int rc = ::poll(&pfd, 1, 1000);
        if (rc <= 0) continue;

        v4l2_buffer b;
        std::memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(fd_, VIDIOC_DQBUF, &b) < 0) continue;
        CamBuf* bufs = (CamBuf*)bufs_;
        if (!bufs || b.index >= nbufs_ || !bufs[b.index].ptr) {
            ::ioctl(fd_, VIDIOC_QBUF, &b);
            continue;
        }
        Frame f;
        f.data.assign((const uint8_t*)bufs[b.index].ptr, (const uint8_t*)bufs[b.index].ptr + b.bytesused);
        f.w = cam_w_;
        f.h = cam_h_;
        f.format = fmt_;
        f.ts_ms = (uint64_t)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
        ::ioctl(fd_, VIDIOC_QBUF, &b);

        std::lock_guard<std::mutex> lk(mu_);
        // 保留协商尺寸
        latest_.w = f.w;
        latest_.h = f.h;
        latest_.format = f.format;
        latest_.data = std::move(f.data);
        latest_.ts_ms = f.ts_ms;

        // 2 秒一次的出帧率统计（debug 级）：确认摄像头实际帧率与瓶颈
        static uint64_t s_frames = 0;
        static auto s_t0 = std::chrono::steady_clock::now();
        s_frames++;
        auto now = std::chrono::steady_clock::now();
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_t0).count();
        if (el >= 2000) {
            CAM_DEBUG("camera %d fps (%dx%d %s)",
                      (int)(s_frames * 1000 / el), cam_w_, cam_h_,
                      fmt_ == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV");
            s_frames = 0;
            s_t0 = now;
        }
    }
}

bool Camera::read_latest(Frame& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (latest_.data.empty()) return false;
    out = latest_;
    return true;
}

// ── JPEG 工具 ──

bool Camera::jpeg_to_rgb(const uint8_t* jpg, size_t len, int& w, int& h,
                         std::vector<uint8_t>& rgb, int max_out_w) {
    jpeg_decompress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpg, len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_RGB;
    unsigned scale = 1;
    if (max_out_w > 0) {
        while ((cinfo.image_width / scale) > (unsigned)max_out_w && scale < 8) scale <<= 1;
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = scale;
    jpeg_start_decompress(&cinfo);
    w = (int)cinfo.output_width;
    h = (int)cinfo.output_height;
    rgb.resize((size_t)w * h * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* row = &rgb[(size_t)cinfo.output_scanline * w * 3];
        jpeg_read_scanlines(&cinfo, (JSAMPARRAY)&row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

bool Camera::rgb_to_jpeg(const uint8_t* rgb, int w, int h, int quality,
                         std::vector<uint8_t>& out) {
    jpeg_compress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_compress(&cinfo);
        return false;
    }
    jpeg_create_compress(&cinfo);

    unsigned char* mem = nullptr;
    unsigned long mem_len = 0;
    jpeg_mem_dest(&cinfo, &mem, &mem_len);

    cinfo.image_width = (JDIMENSION)w;
    cinfo.image_height = (JDIMENSION)h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + (size_t)cinfo.next_scanline * w * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    out.assign(mem, mem + mem_len);
    free(mem);
    return true;
}

void Camera::yuyv_to_rgb(const uint8_t* src, int w, int h, uint8_t* rgb) {
    for (int sy = 0; sy < h; ++sy) {
        for (int sx = 0; sx < w; ++sx) {
            const uint8_t* py = src + ((size_t)sy * w + sx) * 2;
            const uint8_t* pc = src + ((size_t)sy * w + (sx & ~1)) * 2;
            uint8_t Y = py[0], U = pc[1], V = pc[3];
            int C = (int)Y - 16, D = (int)U - 128, E = (int)V - 128;
            int r = (298 * C + 409 * E + 128) >> 8;
            int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int b = (298 * C + 516 * D + 128) >> 8;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            rgb[(size_t)sy * w * 3 + sx * 3 + 0] = (uint8_t)r;
            rgb[(size_t)sy * w * 3 + sx * 3 + 1] = (uint8_t)g;
            rgb[(size_t)sy * w * 3 + sx * 3 + 2] = (uint8_t)b;
        }
    }
}

bool Camera::letterbox_rgb(const uint8_t* rgb, int w, int h,
                           uint8_t* out, int out_w, int out_h) {
    if (w <= 0 || h <= 0 || out_w <= 0 || out_h <= 0) return false;
    double r = std::min((double)out_w / w, (double)out_h / h);
    int new_w = (int)(w * r + 0.5);
    int new_h = (int)(h * r + 0.5);
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;
    int dw = (out_w - new_w) / 2;
    int dh = (out_h - new_h) / 2;
    std::memset(out, 0, (size_t)out_w * out_h * 3);  // 黑边

    for (int y = 0; y < new_h; y++) {
        int sy = (int)(y / r);
        if (sy >= h) sy = h - 1;
        const uint8_t* src_row = rgb + (size_t)sy * w * 3;
        uint8_t* dst_row = out + ((size_t)(dh + y) * out_w + dw) * 3;
        for (int x = 0; x < new_w; x++) {
            int sx = (int)(x / r);
            if (sx >= w) sx = w - 1;
            const uint8_t* p = src_row + (size_t)sx * 3;
            dst_row[x * 3 + 0] = p[0];
            dst_row[x * 3 + 1] = p[1];
            dst_row[x * 3 + 2] = p[2];
        }
    }
    return true;
}

bool Camera::jpeg_get_size(const uint8_t* jpg, size_t len, int& w, int& h) {
    jpeg_decompress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpg, len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    w = (int)cinfo.image_width;
    h = (int)cinfo.image_height;
    jpeg_destroy_decompress(&cinfo);
    return true;
}

Camera& Camera::get_instance() {
    static Camera cam;
    return cam;
}

}  // namespace csrc

#else  // !__linux__ —— 开发机 stub

namespace csrc {

Camera::~Camera() {}

bool Camera::open(int, int, int) {
    CAM_WARN("camera unavailable on non-Linux build");
    return false;
}
void Camera::close() {}
bool Camera::read_latest(Frame&) { return false; }

bool Camera::jpeg_to_rgb(const uint8_t*, size_t, int&, int&, std::vector<uint8_t>&, int) {
    return false;
}
bool Camera::rgb_to_jpeg(const uint8_t*, int, int, int, std::vector<uint8_t>&) {
    return false;
}
void Camera::yuyv_to_rgb(const uint8_t*, int, int, uint8_t*) {}
bool Camera::letterbox_rgb(const uint8_t*, int, int, uint8_t*, int, int) { return false; }
bool Camera::jpeg_get_size(const uint8_t*, size_t, int&, int&) { return false; }

Camera& Camera::get_instance() {
    static Camera cam;
    return cam;
}

}  // namespace csrc

#endif  // __linux__
