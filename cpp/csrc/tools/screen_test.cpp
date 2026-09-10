// csrc/tools/screen_test.cpp — 板载 SPI 屏直测工具（排查屏/接线用）
//
// 用法（板上）：
//   screen_test colors            全屏纯色顺序播放（红/绿/蓝/白 + 字节交换版），
//                                 用于确认面板 RGB565 字节序/通道序与方向
//   screen_test bench             写屏带宽基准（整块 vs 逐行），确认 SPI 有效吞吐
//   screen_test camera [scale]    摄像头实时预览（scale=2 半屏，默认；1=全屏）
//   screen_test info              打印 framebuffer 信息与显示引擎状态
//
// 说明：屏是 SPI 接口，写屏带宽是硬瓶颈。出厂设备树 4MHz ≈ 420KB/s（约 2fps）；
// 实测该板 spi-max-frequency 提到 20MHz ≈ 2MB/s（24MHz 起白屏）。
// 改频见设备树 st7796s@0 节点：spi-max-frequency。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "csrc/camera.hpp"
#include "csrc/log.hpp"
#include "csrc/screen_display.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fb.h>     // struct fb_var_screeninfo（ioctl 必须用真结构体，见下）
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace {

#if defined(__linux__)

struct Fb {
    int fd = -1;
    uint16_t* base = nullptr;
    size_t words = 0, bytes = 0;
    int w = 0, h = 0;

    bool open() {
        fd = ::open("/dev/fb0", O_RDWR);
        if (fd < 0) {
            CAM_ERROR("open /dev/fb0: %s", std::strerror(errno));
            return false;
        }
        // FBIOGET_VSCREENINFO 内核写满 struct fb_var_screeninfo（160B），
        // 必须用真结构体接收（手写小结构体会被越界写踩栈 → 段错误）
        struct fb_var_screeninfo vi;
        std::memset(&vi, 0, sizeof vi);
        if (::ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0 &&
            vi.xres > 0 && vi.yres > 0 && vi.bits_per_pixel > 0) {
            w = (int)vi.xres;
            h = (int)vi.yres;
            words = (size_t)vi.xres * vi.yres;
            bytes = words * (vi.bits_per_pixel / 8);
        } else {
            w = 320; h = 480;
            words = (size_t)w * h;
            bytes = words * 2;
        }
        void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            CAM_ERROR("mmap /dev/fb0: %s", std::strerror(errno));
            ::close(fd); fd = -1;
            return false;
        }
        base = (uint16_t*)p;
        return true;
    }
    void close() {
        if (base) { ::munmap(base, bytes); base = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    void fill(uint16_t c) { for (size_t i = 0; i < words; i++) base[i] = c; }
};

void enable_display_engine() {
    if (FILE* s = fopen("/sys/class/graphics/fb0/state", "w")) { fputs("1", s); fclose(s); }
    if (FILE* b = fopen("/sys/class/graphics/fb0/blank", "w")) { fputs("0", b); fclose(b); }
}

long ms_between(const std::chrono::steady_clock::time_point& a,
                const std::chrono::steady_clock::time_point& b) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

int cmd_info() {
    Fb fb;
    if (!fb.open()) return 1;
    printf("framebuffer : %dx%d, %zu words, %zu bytes\n", fb.w, fb.h, fb.words, fb.bytes);
    for (const char* p : {"/sys/class/graphics/fb0/state", "/sys/class/graphics/fb0/blank"}) {
        if (FILE* f = fopen(p, "r")) {
            char buf[32] = {0};
            if (fgets(buf, sizeof buf, f)) {
                std::string s(buf);
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                printf("%-28s: %s\n", p, s.c_str());
            }
            fclose(f);
        }
    }
    fb.close();
    return 0;
}

/// 全屏纯色顺序播放：确认面板颜色解析（红显示成白/白显示成蓝等 → 屏非标准 RGB565）
int cmd_colors() {
    Fb fb;
    if (!fb.open()) return 1;
    enable_display_engine();
    static const uint16_t kColors[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    static const char* kNames[4] = {"RED", "GREEN", "BLUE", "WHITE"};
    int seq = 1;
    for (int phase = 0; phase < 2; phase++) {   // 0=原值 1=高低字节交换
        for (int i = 0; i < 4; i++) {
            uint16_t v = kColors[i];
            if (phase) v = (uint16_t)((v >> 8) | (v << 8));
            fb.fill(v);
            printf("TEST #%d [%s] %s -> 0x%04X\n", seq++, phase ? "byte-swap" : "normal",
                   kNames[i], v);
            fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    }
    fb.fill(0x0000);
    fb.close();
    return 0;
}

/// 写屏带宽基准：整块 vs 逐行（判断驱动"每次调用"固定开销）
int cmd_bench() {
    Fb fb;
    if (!fb.open()) return 1;
    enable_display_engine();
    const size_t n = fb.words;
    std::vector<uint16_t> buf(n);
    uint32_t seed = 0x12345678;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        buf[i] = (uint16_t)(seed >> 16);
    }
    long best;
    // A: 整块
    best = 0;
    for (int k = 0; k < 3; k++) {
        auto t0 = std::chrono::steady_clock::now();
        std::memcpy(fb.base, buf.data(), fb.bytes);
        long ms = ms_between(t0, std::chrono::steady_clock::now());
        if (best == 0 || ms < best) best = ms;
    }
    printf("A full memcpy  %zu bytes: %ldms = %ld KB/s\n", fb.bytes, best,
           best ? (long)(fb.bytes / 1024 / best) : 0);
    // B: 逐行
    best = 0;
    for (int k = 0; k < 3; k++) {
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < fb.h; r++)
            std::memcpy(fb.base + (size_t)r * fb.w, buf.data() + (size_t)r * fb.w,
                        (size_t)fb.w * 2);
        long ms = ms_between(t0, std::chrono::steady_clock::now());
        if (best == 0 || ms < best) best = ms;
    }
    printf("B row-wise    %zu bytes: %ldms = %ld KB/s\n", fb.bytes, best,
           best ? (long)(fb.bytes / 1024 / best) : 0);
    fb.fill(0x0000);
    fb.close();
    return 0;
}

int cmd_camera(int scale) {
    csrc::Camera& cam = csrc::Camera::get_instance();
    if (!cam.open(640, 360, 15)) {
        CAM_ERROR("camera open failed");
        return 1;
    }
    csrc::ScreenDisplay disp;
    csrc::DisplayConfig dc;
    dc.scale = scale;
    dc.orient = 3;
    dc.fps = 15;
    dc.decode_max_w = 320;
    if (!disp.start(dc)) {
        CAM_ERROR("display start failed (no /dev/fb0?)");
        cam.close();
        return 1;
    }
    printf("camera preview running, Ctrl-C to stop\n");
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        csrc::ScreenDisplay::Stats st = disp.stats();
        printf("%d fps | dec %dms | conv %dms | blit %dms | rows %d/%d | region %dx%d\n",
               st.fps, st.dec_ms, st.conv_ms, st.blit_ms, st.rows, st.total_rows,
               st.out_w, st.out_h);
        fflush(stdout);
    }
    return 0;
}

#endif  // __linux__

}  // namespace

int main(int argc, char** argv) {
#if defined(__linux__)
    CAM_INFO("screen_test — 板载 SPI 屏直测");
    std::string cmd = argc > 1 ? argv[1] : "info";
    if (cmd == "info") return cmd_info();
    if (cmd == "colors") return cmd_colors();
    if (cmd == "bench") return cmd_bench();
    if (cmd == "camera") {
        int scale = argc > 2 ? atoi(argv[2]) : 2;
        return cmd_camera(scale);
    }
    printf("usage: screen_test info|colors|bench|camera [scale]\n");
    return 1;
#else
    (void)argc; (void)argv;
    CAM_WARN("screen_test is Linux-only (no /dev/fb0)");
    return 1;
#endif
}
