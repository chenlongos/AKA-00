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

    struct Frame {
        std::vector<uint8_t> data;
        int w = 0, h = 0;
        uint32_t format = 0;      // V4L2_PIX_FMT_MJPEG / V4L2_PIX_FMT_YUYV（0 = 未知）
        uint64_t ts_ms = 0;       // 采集时间戳（毫秒）
    };

    /// 拷贝最新帧。返回 true 且有数据。
    bool read_latest(Frame& out);

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

    bool available_ = false;
    bool running_ = false;
    std::thread* thread_ = nullptr;

    mutable std::mutex mu_;
    Frame latest_;

    // Linux V4L2 状态（非 Linux 编译时不定义）
    int fd_ = -1;
    void* bufs_ = nullptr;   // CamBuf*
    unsigned nbufs_ = 0;
    uint32_t fmt_ = 0;
    int cam_w_ = 0, cam_h_ = 0;   // 协商分辨率（capture_loop 上报用）
};

}  // namespace csrc
