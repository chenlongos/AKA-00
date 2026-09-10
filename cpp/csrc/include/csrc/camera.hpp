// csrc/camera.hpp — V4L2 + libjpeg 摄像头驱动（参考 tests/demo_camera.c）
//
// 采集: V4L2 mmap 双缓冲 + poll + DQBUF/QBUF（MJPEG 优先，YUYV 回退）
// 解码: libjpeg（大图自动降采样，错误桩 longjmp 防坏帧拖死）
// 编码: libjpeg jpeg_mem_dest（快照 / MJPEG 流用）
// 采集线程只保留最新帧，read_latest 返回引用拷贝。
//
// 非 Linux 编译目标：全部方法降级为不可用（开发机无摄像头，行为同 Python mock）。

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace csrc {

class Camera {
public:
    Camera() = default;
    ~Camera();

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    /// 打开摄像头并启动采集线程。重复调用幂等。
    bool open(int width, int height, int fps);
    void close();
    bool is_available() const { return available_; }

    /// 固定曝光/白平衡/增益（须在 open() 之前设置）。
    /// 廉价 UVC 摄像头的自动曝光/AWB 会周期性抖动 → 整幅画面每帧一起变
    /// （demo 实测 maxΔ 周期性飙到 50+），屏显示的脏行检测因此失效、写屏流量上升。
    /// 开启后在开流前关掉自动控制并写回当前值（best-effort，不支持的控件忽略）。
    void set_fixed_exposure(bool on) { fixed_exposure_ = on; }
    bool fixed_exposure() const { return fixed_exposure_; }

    struct Frame {
        std::vector<uint8_t> data;
        int w = 0, h = 0;
        uint32_t format = 0;      // V4L2_PIX_FMT_MJPEG / V4L2_PIX_FMT_YUYV（0 = 未知）
        uint64_t ts_ms = 0;       // 采集时间戳（毫秒）
    };

    /// 拷贝最新帧。返回 true 且有数据。
    bool read_latest(Frame& out);

    /// 最新帧的时间戳（0 = 还没有帧）。轻量：不拷贝帧数据，
    /// 供高频轮询的消费者（屏幕显示线程）判断"是否有新帧"再决定是否拷贝/解码。
    uint64_t latest_ts();

    /// 解码后的 RGB 帧（屏幕显示 / 浏览器流共用）
    struct RgbFrame {
        std::vector<uint8_t> data;   // RGB8，行宽 w*3
        int w = 0, h = 0;
        uint64_t ts_ms = 0;          // 对应采集帧时间戳（同一帧多次调用相同）
    };

    /// 取最新帧的解码 RGB8（**带缓存**）：同一 (ts_ms, max_out_w) 只解码一次，
    /// 多个消费者共享（屏幕显示线程与 /api/camera/stream 的重编码路径）。
    /// max_out_w > 0 时按 libjpeg 整数降采样解码（输出宽 ≤ max_out_w）。
    /// 这样"开屏"不会让浏览器变慢：整帧解码由两边共享，浏览器反而省掉一次解码。
    bool latest_rgb(int max_out_w, RgbFrame& out);

    // ── JPEG 工具（供 capp 路由复用）──

    /// 解码 JPEG → RGB8（w*h*3）。max_out_w>0 时自动降采样解码（输出宽 ≤ max_out_w）。
    static bool jpeg_to_rgb(const uint8_t* jpg, size_t len, int& w, int& h,
                            std::vector<uint8_t>& rgb, int max_out_w = 0);

    /// RGB8 → JPEG（quality 0..100）。
    static bool rgb_to_jpeg(const uint8_t* rgb, int w, int h, int quality,
                            std::vector<uint8_t>& out);

    /// YUYV(4:2:2) → RGB8。每 2 像素共享 U/V，必须从宏像素 (sx & ~1) 取。
    static void yuyv_to_rgb(const uint8_t* src, int w, int h, uint8_t* rgb);

    /// 等比缩放 + 黑边填充到 out_w x out_h（letterbox，Python cv2 版本对齐）。
    static bool letterbox_rgb(const uint8_t* rgb, int w, int h,
                              uint8_t* out, int out_w, int out_h);

    /// 从 JPEG 头读取宽高（不整帧解码）。
    static bool jpeg_get_size(const uint8_t* jpg, size_t len, int& w, int& h);

    /// 判断字节流是否为 JPEG（FF D8）。
    static bool is_jpeg(const uint8_t* p, size_t n) {
        return n >= 2 && p[0] == 0xFF && p[1] == 0xD8;
    }

    /// 获取单例（幂等，重复 open 不重建）。
    static Camera& get_instance();

private:
    void capture_loop();
    bool open_device(int width, int height, int fps);
    void apply_fixed_exposure();   // fixed_exposure_ 为真时在开流前调用

    bool available_ = false;
    bool running_ = false;
    bool fixed_exposure_ = false;   // set_fixed_exposure() → open_device 时应用
    std::thread* thread_ = nullptr;

    mutable std::mutex mu_;
    Frame latest_;

    // 解码缓存（latest_rgb）：同一帧只解码一次，多消费者共享
    std::mutex rgb_mu_;
    RgbFrame rgb_cache_;
    int rgb_cache_max_w_ = -1;

    // Linux V4L2 状态（非 Linux 编译时不定义）
    int fd_ = -1;
    void* bufs_ = nullptr;   // CamBuf*
    unsigned nbufs_ = 0;
    uint32_t fmt_ = 0;
    int cam_w_ = 0, cam_h_ = 0;   // 协商分辨率（capture_loop 上报用）
};

}  // namespace csrc
