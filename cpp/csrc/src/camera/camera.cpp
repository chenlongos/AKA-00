// csrc/camera.cpp — 摄像头设备：V4L2 打开/采集循环/取最新帧
//
// 图像换算（JPEG ↔ RGB / YUYV / letterbox）在 camera/image_convert.cpp；
// 非 Linux 目标的桩在各自文件的 #else 分支里（开发机能编、跑不了）。

#include "csrc/camera.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
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

// ── Starry 内核私有 UVC ioctl（/dev/cvi-usb-camera*）──
// 非标准 V4L2：INIT + GET_INFO + GET_FRAME 三步出帧，命令号是 Starry 驱动的裸整数。
// 交叉编译用 riscv64-linux-musl，__linux__ 恒为真，故 CVI 代码与 V4L2 编进同一份二进制，
// 运行时按设备路径前缀分流（Linux 上不存在 cvi-* 节点，自然走 V4L2 分支）。
constexpr int CVI_CAMERA_IOCTL_INIT = 1;
constexpr int CVI_CAMERA_IOCTL_GET_INFO = 2;
constexpr int CVI_CAMERA_IOCTL_GET_FRAME = 3;

struct CameraInfo {
    uint16_t width;
    uint16_t height;
    uint8_t format;     // 1 = MJPEG
    uint8_t connected;  // 1 = 已连接
};

constexpr size_t FRAME_BUF_SIZE = 640 * 480 * 2;  // MJPEG 不可能超过未压缩 YUV

// 设备路径前缀 → 实现分流（运行时判断，一套二进制双内核）
bool is_cvi_device(const std::string& device) {
    return device.find("/dev/cvi-usb-camera") != std::string::npos;
}

// libjpeg 错误桩：setjmp/longjmp 跳出解码/编码
}  // namespace

namespace csrc {

Camera::~Camera() { close(); }

bool Camera::open_device(const std::string& device, int width, int height, int fps) {
    // 容错：配置的节点不存在时，探测另一内核的默认节点 —— 避免“同一份 config.toml
    // 换内核就没画面”（Linux 默认 /dev/video0，Starry 用 /dev/cvi-usb-camera0）。
    std::string dev = device;
    if (::access(dev.c_str(), F_OK) != 0) {
        const char* alt = is_cvi_device(dev) ? "/dev/video0" : "/dev/cvi-usb-camera0";
        if (::access(alt, F_OK) == 0) {
            CAM_INFO("camera: %s 不存在，回退到 %s", dev.c_str(), alt);
            dev = alt;
        }
    }
    cvi_mode_ = is_cvi_device(dev);

    // ═══════════════ Starry CVI 私有 ioctl 路径 ═══════════════
    if (cvi_mode_) {
        // CVI 不需要 O_NONBLOCK：INIT/GET_INFO/GET_FRAME 均为同步 ioctl。
        fd_ = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            CAM_ERROR("camera(CVI): open %s failed: %s", dev.c_str(), std::strerror(errno));
            return false;
        }
        // INIT：USB 枚举 + UVC 协商 + 启动流 + 丢弃首帧预热（内核内阻塞完成）
        if (::ioctl(fd_, CVI_CAMERA_IOCTL_INIT, 0) < 0) {
            CAM_ERROR("camera(CVI): ioctl INIT failed: %s", std::strerror(errno));
            ::close(fd_); fd_ = -1; return false;
        }
        // GET_INFO：取实际协商到的分辨率/格式/连接状态
        CameraInfo info{};
        if (::ioctl(fd_, CVI_CAMERA_IOCTL_GET_INFO, &info) < 0) {
            CAM_ERROR("camera(CVI): ioctl GET_INFO failed: %s", std::strerror(errno));
            ::close(fd_); fd_ = -1; return false;
        }
        if (!info.connected) {
            CAM_ERROR("camera(CVI): %s reports disconnected", dev.c_str());
            ::close(fd_); fd_ = -1; return false;
        }
        cam_w_ = info.width;
        cam_h_ = info.height;
        fmt_ = info.format;  // 1 = MJPEG；下游以 is_jpeg(实际字节) 判定，此值仅上报用
        CAM_INFO("camera(CVI): %s ready %ux%u fmt=%u",
                 dev.c_str(), info.width, info.height, info.format);
        return true;
    }

    // ═══════════════ Linux V4L2 路径（完整保留）═══════════════
    // O_CLOEXEC：设备 fd 不能被 fork/exec 出去。capp 会拉起 wpa_supplicant（ensure_wpa_env）、
    // curl（https 下载）、OTA 脚本……子进程一旦继承，摄像头这种**独占设备**就再也打不开
    // （板上实测：wpa_supplicant 持有 /dev/video0，/api/camera/open 一直失败）；
    // 串口更隐蔽 —— 能被重复打开，但两个进程写同一条串口会互相打乱。
    fd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        CAM_ERROR("V4L2: open %s failed: %s", dev.c_str(), std::strerror(errno));
        return false;
    }
    v4l2_capability cap;
    std::memset(&cap, 0, sizeof cap);
    if (::ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        CAM_ERROR("V4L2: %s not a streaming capture device", dev.c_str());
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    char drv[17] = {0}, crd[33] = {0};
    std::memcpy(drv, cap.driver, 16);
    std::memcpy(crd, cap.card, 32);
    CAM_INFO("V4L2 driver=%s card=%s", drv, crd);

    // 枚举设备真实支持的格式/分辨率（排查"假协商不出帧"用，mjpg 时代调试手法）。
    // 注意：某些廉价 UVC 固件会在列表里塞入不会真正出流的尺寸。
    for (unsigned idx = 0;; idx++) {
        v4l2_fmtdesc fd;
        std::memset(&fd, 0, sizeof fd);
        fd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;   // ENUM_FMT 必须指定 buffer type
        fd.index = idx;
        if (::ioctl(fd_, VIDIOC_ENUM_FMT, &fd) < 0) break;
        char fourcc[5] = {(char)(fd.pixelformat & 0xff),
                          (char)((fd.pixelformat >> 8) & 0xff),
                          (char)((fd.pixelformat >> 16) & 0xff),
                          (char)((fd.pixelformat >> 24) & 0xff), 0};
        std::string sizes;
        v4l2_frmsizeenum fs;
        std::memset(&fs, 0, sizeof fs);
        fs.pixel_format = fd.pixelformat;
        while (::ioctl(fd_, VIDIOC_ENUM_FRAMESIZES, &fs) == 0) {
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                if (!sizes.empty()) sizes += " ";
                sizes += std::to_string(fs.discrete.width) + "x" + std::to_string(fs.discrete.height);
            }
            fs.index++;
        }
        CAM_INFO("V4L2 fmt %s (%s) supports: %s", fourcc, fd.description,
                 sizes.empty() ? "?" : sizes.c_str());
    }

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
    // 固定曝光/AWB/增益（可选）：必须在开流前设置
    if (fixed_exposure_) apply_fixed_exposure();

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

// 固定曝光/AWB/增益（best-effort）：关自动控制并写回当前值。
// 动机：廉价 UVC 的自动曝光/AWB 周期性抖动 → 整幅画面每帧一起变，屏显示的
// 脏行检测失效（demo 实测 maxΔ 周期性飙到 50+）、写屏流量与帧率下降。
void Camera::apply_fixed_exposure() {
    if (fd_ < 0) return;
    v4l2_control c;
    auto s_ctrl = [&](uint32_t id, int val) -> bool {
        std::memset(&c, 0, sizeof c);
        c.id = id;
        c.value = val;
        return ::ioctl(fd_, VIDIOC_S_CTRL, &c) == 0;
    };
    auto g_ctrl = [&](uint32_t id, int* out) -> bool {
        std::memset(&c, 0, sizeof c);
        c.id = id;
        if (::ioctl(fd_, VIDIOC_G_CTRL, &c) != 0) return false;
        *out = c.value;
        return true;
    };

    const bool ok_awb = s_ctrl(V4L2_CID_AUTO_WHITE_BALANCE, 0);
    int gain = 0;
    bool ok_gain = false;
    if (g_ctrl(V4L2_CID_GAIN, &gain)) {
        s_ctrl(V4L2_CID_AUTOGAIN, 0);            // 手动增益
        ok_gain = s_ctrl(V4L2_CID_GAIN, gain);   // 写回固定增益
    }
    int exp = 0;
    bool ok_exp = false;
    if (g_ctrl(V4L2_CID_EXPOSURE_ABSOLUTE, &exp)) {
        s_ctrl(V4L2_CID_EXPOSURE_AUTO, 1);                  // V4L2_EXPOSURE_MANUAL
        ok_exp = s_ctrl(V4L2_CID_EXPOSURE_ABSOLUTE, exp);   // 写回固定曝光
    }
    s_ctrl(V4L2_CID_BACKLIGHT_COMPENSATION, 0);
    CAM_INFO("[camera] 固定曝光: AWB=%s 增益=%s%s 曝光=%s%s 背光补偿=关",
             ok_awb ? "关" : "不支持",
             ok_gain ? "固定" : "不支持", ok_gain ? ("(" + std::to_string(gain) + ")").c_str() : "",
             ok_exp ? "固定" : "不支持", ok_exp ? ("(" + std::to_string(exp) + ")").c_str() : "");
}

void Camera::close() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    close_device();
}

void Camera::close_device() {
    available_ = false;
    if (thread_) {
        running_ = false;
        thread_->join();
        delete thread_;
        thread_ = nullptr;
    }
    if (fd_ >= 0) {
        if (!cvi_mode_) {
            v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ::ioctl(fd_, VIDIOC_STREAMOFF, &t);
        }
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
    {
        std::lock_guard<std::mutex> lk(mu_);
        latest_ = Frame{};
    }
    {
        std::lock_guard<std::mutex> lk(rgb_mu_);
        rgb_cache_ = RgbFrame{};
        rgb_cache_max_w_ = -1;
    }
}

bool Camera::open(const std::string& device, int width, int height, int fps) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mu_);
    if (available_) return true;
    if (!open_device(device, width, height, fps)) {
        close_device();
        return false;
    }
    running_ = true;
    try {
        thread_ = new std::thread([this] { capture_loop(); });
    } catch (const std::exception& error) {
        running_ = false;
        CAM_ERROR("camera capture thread: %s", error.what());
        close_device();
        return false;
    }
    available_ = true;
    CAM_INFO("camera(%s) capture thread started (%dx%d)",
             cvi_mode_ ? "CVI" : "V4L2", cam_w_, cam_h_);
    return true;
}

void Camera::capture_loop() {
    if (cvi_mode_) {
        // ── Starry CVI 采集循环 ──
        // GET_FRAME 是同步阻塞 ioctl：内核采一帧 MJPEG 写入 buf 并返回字节数。
        std::vector<uint8_t> buf(FRAME_BUF_SIZE);
        static uint64_t s_frames_c = 0;
        static auto s_t0_c = std::chrono::steady_clock::now();
        while (running_) {
            if (fd_ < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            int n = ::ioctl(fd_, CVI_CAMERA_IOCTL_GET_FRAME, buf.data());
            if (n <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            uint64_t ts_ms = (uint64_t)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
            {
                std::lock_guard<std::mutex> lk(mu_);
                latest_.data.assign(buf.data(), buf.data() + n);
                latest_.w = cam_w_;
                latest_.h = cam_h_;
                latest_.format = 1;  // MJPEG
                latest_.ts_ms = ts_ms;
            }
            s_frames_c++;
            auto now = std::chrono::steady_clock::now();
            auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_t0_c).count();
            if (el >= 2000) {
                CAM_DEBUG("camera(CVI) %d fps (%dx%d MJPEG)",
                          (int)(s_frames_c * 1000 / el), cam_w_, cam_h_);
                s_frames_c = 0;
                s_t0_c = now;
            }
        }
        return;
    }

    // ── Linux V4L2 采集循环（完整保留）──
    auto last_good = std::chrono::steady_clock::now();
    bool stall_warned = false;
    while (running_) {
        if (fd_ < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // 无帧看门狗：协商"成功"但 4 秒一帧不出 → 极可能是该尺寸的"假档"
        // （廉价 UVC 常见：列表里有、S_FMT 也接受、但固件根本不出流）。
        auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - last_good)
                           .count();
        if (idle_ms >= 4000 && !stall_warned) {
            stall_warned = true;
            CAM_ERROR("V4L2 stream on but NO frame for %lld ms: negotiated %dx%d %s "
                      "may be a bogus mode (S_FMT ok, no frames). Use a size from the "
                      "'V4L2 fmt ... supports:' list above",
                      (long long)idle_ms, cam_w_, cam_h_,
                      fmt_ == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV");
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
        uint64_t ts_ms = (uint64_t)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
        // 直接写 latest_（锁内 assign 复用其容量），避免每帧新建临时 vector——
        // 长时间运行每帧一次 malloc 会累积碎片并加剧卡顿
        {
            std::lock_guard<std::mutex> lk(mu_);
            latest_.data.assign((const uint8_t*)bufs[b.index].ptr,
                                (const uint8_t*)bufs[b.index].ptr + b.bytesused);
            latest_.w = cam_w_;
            latest_.h = cam_h_;
            latest_.format = fmt_;
            latest_.ts_ms = ts_ms;
        }
        ::ioctl(fd_, VIDIOC_QBUF, &b);
        last_good = std::chrono::steady_clock::now();

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

// 轻量时间戳查询：不拷贝帧数据（显示线程高频轮询用，避免白拷 172KB/次）
uint64_t Camera::latest_ts() {
    std::lock_guard<std::mutex> lk(mu_);
    return latest_.data.empty() ? 0 : latest_.ts_ms;
}

// 带缓存的解码：同一 (ts_ms, max_out_w) 只解码一次，供屏幕显示与浏览器流共享。
// 注意：解码**不持 mu_**（否则会阻塞采集线程写最新帧），只在缓存读写时持 rgb_mu_。
bool Camera::latest_rgb(int max_out_w, RgbFrame& out) {
    Frame f;
    if (!read_latest(f) || f.data.empty()) return false;   // 拷贝最新帧（~30KB，微秒级）

    {
        std::lock_guard<std::mutex> lk(rgb_mu_);
        if (rgb_cache_.ts_ms == f.ts_ms && rgb_cache_max_w_ == max_out_w &&
            !rgb_cache_.data.empty()) {
            out = rgb_cache_;   // 命中：本帧已解过，直接用
            return true;
        }
    }

    RgbFrame rgb;
    rgb.ts_ms = f.ts_ms;
    int w = f.w, h = f.h;
    if (is_jpeg(f.data.data(), f.data.size())) {
        if (!jpeg_to_rgb(f.data.data(), f.data.size(), w, h, rgb.data, max_out_w))
            return false;
    } else if (w > 0 && h > 0) {
        // YUYV 兜底：不支持降采样解码，按原尺寸转
        rgb.data.resize((size_t)w * h * 3);
        yuyv_to_rgb(f.data.data(), w, h, rgb.data.data());
    } else {
        return false;
    }
    rgb.w = w;
    rgb.h = h;

    {
        std::lock_guard<std::mutex> lk(rgb_mu_);
        rgb_cache_ = rgb;             // 存入缓存供其它消费者复用
        rgb_cache_max_w_ = max_out_w;
    }
    out = std::move(rgb);
    return true;
}

Camera& Camera::get_instance() {
    static Camera cam;
    return cam;
}

}  // namespace csrc

#else  // !__linux__ —— 开发机 stub（能编、跑不了）

namespace csrc {

Camera::~Camera() {}

bool Camera::open(const std::string&, int, int, int) {
    CAM_WARN("camera unavailable on non-Linux build");
    return false;
}

void Camera::close() {}

bool Camera::read_latest(Frame&) { return false; }

uint64_t Camera::latest_ts() { return 0; }

bool Camera::latest_rgb(int, RgbFrame&) { return false; }

Camera& Camera::get_instance() {
    static Camera cam;
    return cam;
}

}  // namespace csrc

#endif  // __linux__
