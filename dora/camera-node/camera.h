// camera.h — 跨平台摄像头接口
//   Linux:   V4L2 直接采集 MJPEG
//   macOS:   OpenCV VideoCapture → JPEG 编码
#pragma once

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

constexpr int TARGET_WIDTH  = 640;
constexpr int TARGET_HEIGHT = 480;
constexpr int TARGET_FPS    = 30;

class Camera {
public:
    Camera();
    ~Camera();

    bool open(const char* device = nullptr);
    void close();
    bool good() const;
    int  fd() const;          // Linux V4L2 poll fd，macOS 返回 -1
    int  width()  const;
    int  height() const;
    bool is_mjpeg() const;
    bool wait_frame(int timeout_ms);
    std::pair<const uint8_t*, size_t> read_frame();

private:
    int _width  = TARGET_WIDTH;
    int _height = TARGET_HEIGHT;
    std::vector<uint8_t> _frame;   // 帧缓冲

#ifdef __linux__
    // ── V4L2 成员 ──
    struct Buf { void* ptr; size_t len; };
    int      _fd   = -1;
    uint32_t _fmt  = 0;     // V4L2_PIX_FMT_MJPEG 或 _YUYV
    Buf*     _bufs = nullptr;
    unsigned _n_bufs = 0;

    bool _try_fmt(int& w, int& h);
    void _init_buffers();
    void _start_stream();
    void _stop_stream();
#else
    // ── OpenCV 成员 ──
    void* _cap = nullptr;   // cv::VideoCapture* (不暴露 OpenCV 头文件)
    bool  _warmed = false;
#endif
};
