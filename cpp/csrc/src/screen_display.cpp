// csrc/screen_display.cpp — 摄像头 → /dev/fb0 实时显示实现
//
// 逻辑移植自板上验证过的 tests/demo_camera.c（同一套查表旋转/缩放、脏行检测、
// 方向修正），改为 C++ 类 + 复用 Camera 单例的最新帧与共享解码缓存。
//
// 非 Linux 目标全部降级为 stub（开发机无 /dev/fb0）。

#include "csrc/screen_display.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "csrc/camera.hpp"
#include "csrc/log.hpp"

#if AKA_WITH_SCREEN   // 不带屏版本（-DAKA_WITH_SCREEN=0）整个显示栈不参与编译

#if defined(__linux__)
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <linux/fb.h>     // struct fb_var_screeninfo（FBIOGET_VSCREENINFO 必须用真结构体！）
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

// 脏行容差掩码：忽略 RGB565 每通道最低 N 个 bit。
// 实况视频帧间有传感器噪声（JPEG 量化还会放大），精确比较会把静止画面也判成
// "全行都变"（板上实测 240/240 行全脏），掩掉低 bit 后静态行才真正不重写。
inline uint16_t noise_mask(int n) {
    switch (n) {
        case 0:  return 0xFFFF;
        case 2:  return 0xF3CC;   // R/G/B 各忽略 2 个 LSB
        case 3:  return 0xF1C0;   // R/G/B 各忽略 3 个 LSB
        default: return 0xF7DE;   // R/G/B 各忽略 1 个 LSB
    }
}

/// 开启显示引擎（demo2.c 要求 state=1 才显示；init.sh 也会做一次）
void enable_display_engine() {
    if (FILE* s = fopen("/sys/class/graphics/fb0/state", "w")) {
        fputs("1", s);
        fclose(s);
    }
    if (FILE* b = fopen("/sys/class/graphics/fb0/blank", "w")) {
        fputs("0", b);
        fclose(b);
    }
}

}  // namespace

#endif  // __linux__

namespace csrc {

ScreenDisplay::~ScreenDisplay() { stop(); }

#if defined(__linux__)

bool ScreenDisplay::open_fb() {
    fb_fd_ = ::open("/dev/fb0", O_RDWR);
    if (fb_fd_ < 0) {
        CAM_INFO("[display] no /dev/fb0 (%s) — screen disabled", std::strerror(errno));
        return false;
    }
    // FBIOGET_VSCREENINFO：内核会写入完整 struct fb_var_screeninfo（160 字节），
    // 用真结构体接收。曾用 28 字节的手写小结构体 → 内核越界写 132 字节踩栈，
    // 表现为日志串被改坏 + 随后段错误（板上实测）。
    struct fb_var_screeninfo vi;
    std::memset(&vi, 0, sizeof vi);
    if (::ioctl(fb_fd_, FBIOGET_VSCREENINFO, &vi) == 0 &&
        vi.xres > 0 && vi.yres > 0 && vi.bits_per_pixel > 0) {
        fb_w_ = (int)vi.xres;
        fb_h_ = (int)vi.yres;
        // 行步长：部分驱动有虚拟宽（xres_virtual > xres），此时行间隔按虚拟宽算，
        // 否则子区域写屏会错位/越界。缓冲大小按虚拟区算，保证 mmap 覆盖到。
        int stride = vi.xres_virtual > vi.xres ? (int)vi.xres_virtual : (int)vi.xres;
        int vrows = vi.yres_virtual > vi.yres ? (int)vi.yres_virtual : (int)vi.yres;
        fb_stride_ = stride;
        fb_words_ = (size_t)stride * vrows;
        fb_bytes_ = fb_words_ * (vi.bits_per_pixel / 8);
    } else {
        CAM_WARN("[display] FBIOGET_VSCREENINFO failed, assuming 320x480@16bpp");
        fb_w_ = 320;
        fb_h_ = 480;
        fb_stride_ = fb_w_;
        fb_words_ = (size_t)fb_w_ * fb_h_;
        fb_bytes_ = fb_words_ * 2;
    }
    void* p = ::mmap(nullptr, fb_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0);
    if (p == MAP_FAILED) {
        CAM_ERROR("[display] mmap /dev/fb0 failed: %s", std::strerror(errno));
        ::close(fb_fd_);
        fb_fd_ = -1;
        return false;
    }
    fb_ = (uint16_t*)p;
    CAM_INFO("[display] fb0 %dx%d mapped (%zu bytes)", fb_w_, fb_h_, fb_bytes_);
    return true;
}

void ScreenDisplay::close_fb() {
    if (fb_) {
        ::munmap(fb_, fb_bytes_);
        fb_ = nullptr;
    }
    if (fb_fd_ >= 0) {
        ::close(fb_fd_);
        fb_fd_ = -1;
    }
}

bool ScreenDisplay::start(const DisplayConfig& cfg) {
    if (running_) return true;
    cfg_ = cfg;

    // 归一化参数
    if (cfg_.scale < 1) cfg_.scale = 1;
    if (cfg_.scale > 4) cfg_.scale = 4;
    if (cfg_.orient < 0) cfg_.orient = 0;
    if (cfg_.orient > 3) cfg_.orient = 3;
    if (cfg_.fps < 1) cfg_.fps = 1;
    if (cfg_.fps > 60) cfg_.fps = 60;
    if (cfg_.noise < 0) cfg_.noise = 0;
    if (cfg_.noise > 3) cfg_.noise = 3;
    if (cfg_.decode_max_w < 0) cfg_.decode_max_w = 0;

    enable_display_engine();
    if (!open_fb()) return false;

    out_w_ = fb_w_ / cfg_.scale;
    out_h_ = fb_h_ / cfg_.scale;
    if (out_w_ < 8) out_w_ = 8;
    if (out_h_ < 8) out_h_ = 8;

    blit_dst_ = fb_ + (size_t)((fb_h_ - out_h_) / 2) * fb_stride_ + (fb_w_ - out_w_) / 2;
    buf_.assign((size_t)out_w_ * out_h_, 0);
    // 首帧强制全量上屏：prev 全 0xFF 与任何真实画面都不等
    prev_.assign((size_t)out_w_ * out_h_, 0xFFFF);
    sy_map_.assign((size_t)out_w_, 0);
    sx_map_.assign((size_t)out_h_, 0);

    {
        std::lock_guard<std::mutex> lk(st_mu_);
        st_ = Stats{};
        st_.out_w = out_w_;
        st_.out_h = out_h_;
        st_.total_rows = out_h_;
        st_.fb_w = fb_w_;
        st_.fb_h = fb_h_;
    }

    running_ = true;
    thread_ = new std::thread([this] { loop(); });
    CAM_INFO("[display] ▶ %dx%d region (1/%d screen), orient=%d, fps<=%d, noise=%d",
             out_w_, out_h_, cfg_.scale, cfg_.orient, cfg_.fps, cfg_.noise);
    return true;
}

void ScreenDisplay::stop() {
    if (thread_) {
        running_ = false;
        thread_->join();
        delete thread_;
        thread_ = nullptr;
    }
    close_fb();
    blit_dst_ = nullptr;
    CAM_INFO("[display] ⏸  stopped");
}

/// RGB8 → RGB565：旋转 90°（顺时针）+ cover 缩放居中裁切到显示区。
///
/// 源 w x h 的 RGB8（行宽 w*3）；旋转后尺寸为 h x w。
/// 逐像素反推：
///   输出列 ox → 源行 sy（越靠右=源越靠上），输出行 oy → 源列 sx。
/// 反推映射只依赖 ox 或 oy，预先算成整数查表；riscv64 软浮点下逐像素除法极慢，
/// 查表后内层只剩整数运算（板上实测这一步 ~7ms/帧）。
void ScreenDisplay::convert(const uint8_t* rgb, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const double sw = (double)h;      // 旋转后宽
    const double sh = (double)w;      // 旋转后高
    const double s = (out_w_ / sw) > (out_h_ / sh) ? (out_w_ / sw) : (out_h_ / sh);
    const double off_x = (sw * s - out_w_) / 2.0;
    const double off_y = (sh * s - out_h_) / 2.0;
    const size_t stride = (size_t)w * 3;

    for (int ox = 0; ox < out_w_; ++ox) {
        int sy = (int)(h - 1 - (off_x + ox) / s + 0.5);
        if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;
        sy_map_[ox] = sy;
    }
    for (int oy = 0; oy < out_h_; ++oy) {
        int sx = (int)((off_y + oy) / s + 0.5);
        if (sx < 0) sx = 0; else if (sx >= w) sx = w - 1;
        sx_map_[oy] = sx;
    }

    for (int oy = 0; oy < out_h_; ++oy) {
        const size_t col_off = (size_t)sx_map_[oy] * 3;   // 源列字节偏移
        // 方向修正：位0=水平翻转(ox)，位1=垂直翻转(oy)
        const int dst_oy = (cfg_.orient & 2) ? (out_h_ - 1 - oy) : oy;
        uint16_t* dst = buf_.data() + (size_t)dst_oy * out_w_;
        for (int ox = 0; ox < out_w_; ++ox) {
            const int dst_ox = (cfg_.orient & 1) ? (out_w_ - 1 - ox) : ox;
            const uint8_t* p = rgb + (size_t)sy_map_[ox] * stride + col_off;
            dst[dst_ox] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
}

/// 脏行写屏：只把内容变化的行写进 fb0（屏按行扫描，静态区域完全不重写）。
/// 行间必须按屏幕行宽 fb_w_ 跳，不能整块连续拷贝（否则图像在屏上斜着拼）。
void ScreenDisplay::blit_dirty(int& dirty_rows) {
    dirty_rows = 0;
    const uint16_t mask = noise_mask(cfg_.noise);
    for (int r = 0; r < out_h_; ++r) {
        const uint16_t* cur = buf_.data() + (size_t)r * out_w_;
        uint16_t* pv = prev_.data() + (size_t)r * out_w_;
        bool changed = false;
        for (int i = 0; i < out_w_; ++i) {
            if (((cur[i] ^ pv[i]) & mask) != 0) { changed = true; break; }
        }
        if (!changed) continue;
        std::memcpy(blit_dst_ + (size_t)r * fb_stride_, cur, (size_t)out_w_ * sizeof(uint16_t));
        std::memcpy(pv, cur, (size_t)out_w_ * sizeof(uint16_t));
        dirty_rows++;
    }
}

void ScreenDisplay::loop() {
    Camera& cam = Camera::get_instance();
    const auto interval = std::chrono::milliseconds(1000 / cfg_.fps);
    auto last_push = std::chrono::steady_clock::now() - interval;
    uint64_t last_ts = 0;

    // 1Hz 统计窗口
    auto win_t0 = std::chrono::steady_clock::now();
    int win_frames = 0, win_rows = 0;
    long long win_dec = 0, win_conv = 0, win_blit = 0;

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        // 帧率上限：到点才处理（避免抢占 Web 服务/浏览器流的 CPU）
        auto now = std::chrono::steady_clock::now();
        if (now - last_push < interval) continue;

        // 先做轻量时间戳判断：没有新帧就完全不拷贝、不解码（否则每 2ms 白拷一次帧）
        uint64_t ts = cam.latest_ts();
        if (ts == 0 || ts == last_ts) continue;

        // 复用 Camera 单例 + 共享解码缓存：同一帧屏幕与浏览器只解码一次。
        // 命中缓存时 dec 接近 0（说明浏览器已经解过这一帧）。
        auto t0 = std::chrono::steady_clock::now();
        Camera::RgbFrame rgb;
        if (!cam.latest_rgb(cfg_.decode_max_w, rgb) || rgb.data.empty()) continue;
        auto t1 = std::chrono::steady_clock::now();
        if (rgb.ts_ms == last_ts) continue;   // 竞态兜底：与上面判断之间换了帧也无妨
        last_ts = rgb.ts_ms;

        convert(rgb.data.data(), rgb.w, rgb.h);
        auto t2 = std::chrono::steady_clock::now();

        int dirty = 0;
        if (blit_dst_) blit_dirty(dirty);
        auto t3 = std::chrono::steady_clock::now();

        // 首帧日志：确认显示链路真的通了（解码尺寸 → 显示区 → 实际写屏行数）
        static bool first_frame_logged = false;
        if (!first_frame_logged) {
            first_frame_logged = true;
            CAM_INFO("[display] 首帧已上屏 %dx%d → %dx%d 区, %d/%d 行 (dec %lldms conv %lldms blit %lldms)",
                     rgb.w, rgb.h, out_w_, out_h_, dirty, out_h_,
                     (long long)(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000),
                     (long long)(std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / 1000),
                     (long long)(std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count() / 1000));
        }

        win_frames++;
        win_rows += dirty;
        win_dec += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        win_conv += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
        win_blit += std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
        last_push = t3;

        // 1Hz 上报（debug 级：默认 info 不刷屏）
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - win_t0).count();
        if (el >= 1000) {
            int n = win_frames > 0 ? win_frames : 1;
            CAM_DEBUG("[display] %d fps | dec %lldms | conv %lldms | blit %lldms (%d/%d rows)",
                      win_frames, win_dec / n / 1000, win_conv / n / 1000,
                      win_blit / n / 1000, win_rows / n, out_h_);
            std::lock_guard<std::mutex> lk(st_mu_);
            st_.fps = win_frames;
            st_.dec_ms = (int)(win_dec / n / 1000);
            st_.conv_ms = (int)(win_conv / n / 1000);
            st_.blit_ms = (int)(win_blit / n / 1000);
            st_.rows = win_rows / n;
            st_.frames += (uint64_t)win_frames;
            win_frames = win_rows = 0;
            win_dec = win_conv = win_blit = 0;
            win_t0 = t3;
        }
    }
}

ScreenDisplay::Stats ScreenDisplay::stats() const {
    std::lock_guard<std::mutex> lk(st_mu_);
    return st_;
}

#else  // !__linux__ —— 开发机 stub

bool ScreenDisplay::open_fb() { return false; }
void ScreenDisplay::close_fb() {}
bool ScreenDisplay::start(const DisplayConfig& cfg) {
    cfg_ = cfg;
    CAM_WARN("[display] screen unavailable on non-Linux build");
    return false;
}
void ScreenDisplay::stop() {}
void ScreenDisplay::convert(const uint8_t*, int, int) {}
void ScreenDisplay::blit_dirty(int& rows) { rows = 0; }
void ScreenDisplay::loop() {}
ScreenDisplay::Stats ScreenDisplay::stats() const { return st_; }

#endif  // __linux__

#else  // !AKA_WITH_SCREEN —— 不带屏版本：整个显示栈编译期裁掉

namespace csrc {

ScreenDisplay::~ScreenDisplay() {}
bool ScreenDisplay::open_fb() { return false; }
void ScreenDisplay::close_fb() {}
bool ScreenDisplay::start(const DisplayConfig& cfg) {
    cfg_ = cfg;
    CAM_INFO("[display] 本版本未包含屏显示（AKA_WITH_SCREEN=0）");
    return false;
}
void ScreenDisplay::stop() {}
void ScreenDisplay::convert(const uint8_t*, int, int) {}
void ScreenDisplay::blit_dirty(int& rows) { rows = 0; }
void ScreenDisplay::loop() {}
ScreenDisplay::Stats ScreenDisplay::stats() const { return st_; }

#endif  // AKA_WITH_SCREEN

}  // namespace csrc
