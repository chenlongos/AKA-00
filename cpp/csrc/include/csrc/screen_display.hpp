// csrc/screen_display.hpp — 摄像头 → 板载 SPI 屏实时显示（/dev/fb0）
//
// 板上实测结论（ST7796S 320x480 RGB565，见 tests/demo_camera.c 的验证过程）：
//   - 屏是 SPI 接口，写屏带宽是硬瓶颈：出厂 4MHz 只有 ~420KB/s（2fps），
//     实测该板稳定上限 20MHz（~2MB/s）。改频要动设备树 spi-max-frequency。
//   - 全屏 307KB/帧 → 受带宽限制；半屏（160x240）75KB/帧 → 可吃满摄像头 30fps。
//   - 逐行「脏行检测」（带噪声容差）只在内容变化的行写屏，静态画面几乎零流量。
//   - RGB8→RGB565 用整数查表做旋转+cover 缩放，riscv64 上避免逐像素浮点。
//
// 与 Web 服务的关系（关键约束：不能拖慢浏览器看摄像头）：
//   屏幕线程复用 Camera 单例的最新帧，并且**与浏览器共用同一份解码结果**
//   （Camera::latest_rgb 带缓存，同一帧只解码一次）。因此：
//     - 不开屏：浏览器路径与原来完全一致（自己解码 + 重编码）
//     - 开屏后：整帧解码由屏幕线程与浏览器共享 → 浏览器反而省掉一次解码
//   屏幕新增的开销只有 RGB565 转换 + 脏行写屏（~5~10ms/帧，可配 fps 上限）。
//
// 非 Linux 编译目标：全部方法降级为不可用（开发机无 /dev/fb0）。

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "csrc/config.hpp"   // DisplayConfig（config.toml [display]）

// 编译期开关：不带屏版本用 -DAKA_WITH_SCREEN=0 构建（见 cpp/Makefile 的 noscreen 目标），
// 此时整个显示栈被裁掉（二进制更小、不碰 /dev/fb0），API 保持原样（start() 直接返回 false）。
// 带屏版本：cpp/Makefile 默认目标 / `make screen`（输出 aka-capp + aka-capp.tar.gz）。
// 不带屏版本：`make noscreen`（输出 aka-capp-noscreen + aka-capp-noscreen.tar.gz）。
#ifndef AKA_WITH_SCREEN
#define AKA_WITH_SCREEN 1
#endif

namespace csrc {

class ScreenDisplay {
public:
    ScreenDisplay() = default;
    ~ScreenDisplay();

    ScreenDisplay(const ScreenDisplay&) = delete;
    ScreenDisplay& operator=(const ScreenDisplay&) = delete;

    /// 启动显示线程（幂等）。返回 false = 无可用 framebuffer（无屏板不影响服务运行）。
    bool start(const DisplayConfig& cfg);
    /// 停止显示线程并释放 framebuffer（会先清屏，屏变黑）。
    void stop();
    bool running() const { return running_; }
    /// 清屏（把 framebuffer 填黑）。stop() 内部会自动调用。
    void clear();
    /// 不启动显示线程、只清一次屏：开机时摄像头未开 → 屏保持黑，
    /// 等摄像头打开（/api/camera/open）再由显示线程出图。
    static bool clear_screen_once();
    /// /dev/fb0 是否已映射成功（有屏板）
    bool available() const {
#if defined(__linux__)
        return fb_ != nullptr;
#else
        return false;
#endif
    }
    const DisplayConfig& config() const { return cfg_; }

    /// 运行统计（供 /api/display/status 观测）
    struct Stats {
        uint64_t frames = 0;   // 累计上屏帧数
        int fps = 0;           // 最近 1 秒实际上屏帧率
        int dec_ms = 0;        // 取解码帧平均耗时（命中共享缓存时接近 0）
        int conv_ms = 0;       // RGB565 转换平均耗时
        int blit_ms = 0;       // 脏行写屏平均耗时
        int rows = 0;          // 平均脏行数
        int total_rows = 0;    // 显示区总行数
        int out_w = 0, out_h = 0;   // 显示区尺寸
        int fb_w = 0, fb_h = 0;     // 屏幕分辨率
    };
    Stats stats() const;

private:
    void loop();
    bool open_fb();
    void close_fb();
    void convert(const uint8_t* rgb, int w, int h);
    void blit_dirty(int& dirty_rows);

    DisplayConfig cfg_;
    std::atomic<bool> running_{false};

    // 帧缓冲与查表（buf_/prev_/sy_map_/sx_map_ 在 stub 下也要存在，供 convert/blit 空实现）
    std::vector<uint16_t> buf_;    // 当前帧 RGB565（显示区）
    std::vector<uint16_t> prev_;   // 上一帧已上屏内容（脏行检测）
    std::vector<int> sy_map_;      // 输出列 ox → 源行 sy
    std::vector<int> sx_map_;      // 输出行 oy → 源列 sx

    mutable std::mutex st_mu_;
    Stats st_;

#if defined(__linux__)
    // 以下成员仅 Linux 有 /dev/fb0 时使用（非 Linux 编译时整体裁掉）
    std::thread* thread_ = nullptr;

    int fb_fd_ = -1;
    uint16_t* fb_ = nullptr;
    size_t fb_words_ = 0, fb_bytes_ = 0;
    int fb_w_ = 0, fb_h_ = 0;
    int fb_stride_ = 0;      // 行步长（px）：xres_virtual > xres 时用虚拟宽
    uint16_t* blit_dst_ = nullptr;
    int out_w_ = 0, out_h_ = 0;
#endif
};

}  // namespace csrc
