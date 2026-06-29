/// dora C++ 摄像头节点
///   Linux:   V4L2 直接采集 MJPEG，原样输出（无需 OpenCV）
///   macOS:   OpenCV VideoCapture 采集 → JPEG 编码输出（demo 模式）
///
/// 摄像头在 stop 时彻底释放设备，start 时重新打开

#include "node_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>

constexpr int TARGET_WIDTH  = 640;
constexpr int TARGET_HEIGHT = 480;
constexpr int TARGET_FPS    = 30;

// ═══════════════════════════════════════════════════════════════════════════════
//  Linux: V4L2 摄像头 (参考 tests/bench_camera_v4l2.cpp)
// ═══════════════════════════════════════════════════════════════════════════════
#ifdef __linux__

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CLEAR(x) std::memset(&(x), 0, sizeof(x))

class Camera {
public:
    Camera() {}

    bool open(const char* device = "/dev/video0") {
        _fd = ::open(device, O_RDWR | O_NONBLOCK);
        if (_fd < 0) {
            std::fprintf(stderr, "[camera-cpp] ERROR: cannot open %s: %s\n", device, std::strerror(errno));
            return false;
        }

        v4l2_capability cap; CLEAR(cap);
        if (ioctl(_fd, VIDIOC_QUERYCAP, &cap) < 0) {
            std::fprintf(stderr, "[camera-cpp] ERROR: VIDIOC_QUERYCAP: %s\n", std::strerror(errno));
            close(); return false;
        }
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
            std::fprintf(stderr, "[camera-cpp] ERROR: %s not a streaming capture device\n", device);
            close(); return false;
        }

        // MJPEG → 若不支持 640x480 则切 YUYV
        _fmt = V4L2_PIX_FMT_MJPEG;
        _try_fmt(_width, _height);
        if (_width != 640 || _height != 480) {
            std::fprintf(stderr, "[camera-cpp] MJPEG gives %dx%d, retry YUYV for 640x480...\n",
                _width, _height);
            _fmt = V4L2_PIX_FMT_YUYV;
            _width = 640; _height = 480;
            _try_fmt(_width, _height);
        }

        // mmap buffers
        v4l2_requestbuffers req; CLEAR(req);
        req.count = 4; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(_fd, VIDIOC_REQBUFS, &req) < 0) {
            std::fprintf(stderr, "[camera-cpp] ERROR: VIDIOC_REQBUFS: %s\n", std::strerror(errno));
            close(); return false;
        }
        _n_bufs = req.count;
        _buffers = new Buf[_n_bufs];
        for (unsigned i = 0; i < _n_bufs; i++) {
            v4l2_buffer buf; CLEAR(buf);
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
            if (ioctl(_fd, VIDIOC_QUERYBUF, &buf) < 0) { close(); return false; }
            _buffers[i].len = buf.length;
            _buffers[i].ptr = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, buf.m.offset);
            if (_buffers[i].ptr == MAP_FAILED) { close(); return false; }
        }
        for (unsigned i = 0; i < _n_bufs; i++) {
            v4l2_buffer buf; CLEAR(buf);
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
            if (ioctl(_fd, VIDIOC_QBUF, &buf) < 0) { close(); return false; }
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_fd, VIDIOC_STREAMON, &type) < 0) { close(); return false; }

        const char* name = (_fmt == V4L2_PIX_FMT_MJPEG) ? "MJPEG" : "YUYV";
        std::cout << "[camera-cpp] V4L2 " << _width << "x" << _height << " " << name << std::endl;
        return true;
    }

    void close() {
        if (_fd >= 0) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(_fd, VIDIOC_STREAMOFF, &type);
            for (unsigned i = 0; i < _n_bufs; i++)
                if (_buffers[i].ptr && _buffers[i].ptr != MAP_FAILED) munmap(_buffers[i].ptr, _buffers[i].len);
            delete[] _buffers; _buffers = nullptr;
            ::close(_fd); _fd = -1;
        }
        std::cout << "[camera-cpp] V4L2 closed" << std::endl;
    }

    bool good() const { return _fd >= 0; }
    int  fd()   const { return _fd; }
    int  width()  const { return _width; }
    int  height() const { return _height; }
    bool is_mjpeg() const { return _fmt == V4L2_PIX_FMT_MJPEG; }

    bool wait_frame(int timeout_ms) {
        struct pollfd pfd = {_fd, POLLIN, 0};
        return poll(&pfd, 1, timeout_ms) > 0;
    }

    std::pair<const uint8_t*, size_t> read_frame() {
        v4l2_buffer buf; CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(_fd, VIDIOC_DQBUF, &buf) < 0) return {nullptr, 0};
        auto* src = static_cast<const uint8_t*>(_buffers[buf.index].ptr);
        size_t len = buf.bytesused;
        _frame.assign(src, src + len);
        ioctl(_fd, VIDIOC_QBUF, &buf);
        return {_frame.data(), _frame.size()};
    }

private:
    struct Buf { void* ptr; size_t len; };
    int      _fd = -1, _width = TARGET_WIDTH, _height = TARGET_HEIGHT;
    uint32_t _fmt = 0;
    Buf*     _buffers = nullptr;
    unsigned _n_bufs = 0;
    std::vector<uint8_t> _frame;

    bool _try_fmt(int& w, int& h) {
        v4l2_format fmt; CLEAR(fmt);
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = w; fmt.fmt.pix.height = h;
        fmt.fmt.pix.pixelformat = _fmt; fmt.fmt.pix.field = V4L2_FIELD_ANY;
        if (ioctl(_fd, VIDIOC_S_FMT, &fmt) < 0) return false;
        w = fmt.fmt.pix.width; h = fmt.fmt.pix.height;
        return fmt.fmt.pix.pixelformat == _fmt;
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
//  macOS: OpenCV VideoCapture (demo / 开发模式)
// ═══════════════════════════════════════════════════════════════════════════════
#else

#include <opencv2/opencv.hpp>
#include <thread>

class Camera {
public:
    Camera() {}

    bool open(const char* /*unused*/ = nullptr) {
        if (!_cap.open(0)) {
            std::fprintf(stderr, "[camera-cpp] ERROR: cannot open camera 0\n");
            std::fprintf(stderr, "[camera-cpp] macOS: check camera permission in System Settings\n");
            return false;
        }
        _cap.set(cv::CAP_PROP_FRAME_WIDTH,  TARGET_WIDTH);
        _cap.set(cv::CAP_PROP_FRAME_HEIGHT, TARGET_HEIGHT);
        _cap.set(cv::CAP_PROP_FPS, TARGET_FPS);
        // 预热：丢弃前几帧（macOS 摄像头需要时间稳定）
        cv::Mat tmp;
        for (int i = 0; i < 15; i++) _cap >> tmp;
        _width  = (int)_cap.get(cv::CAP_PROP_FRAME_WIDTH);
        _height = (int)_cap.get(cv::CAP_PROP_FRAME_HEIGHT);
        std::cout << "[camera-cpp] OpenCV " << _width << "x" << _height << " ready" << std::endl;
        return true;
    }

    void close() {
        _cap.release();
        std::cout << "[camera-cpp] OpenCV closed" << std::endl;
    }

    bool good()    const { return _cap.isOpened(); }
    int  width()   const { return _width; }
    int  height()  const { return _height; }
    bool is_mjpeg() const { return true; }

    bool wait_frame(int timeout_ms) {
        // VideoCapture 内部已阻塞，简单 sleep 等帧就绪
        if (!_warmed) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); _warmed = true; }
        (void)timeout_ms;
        return good();
    }

    std::pair<const uint8_t*, size_t> read_frame() {
        cv::Mat bgr;
        for (int retry = 0; retry < 5; retry++) {
            _cap >> bgr;
            if (!bgr.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (bgr.empty()) return {nullptr, 0};
        std::vector<uchar> jpeg;
        cv::imencode(".jpg", bgr, jpeg, {cv::IMWRITE_JPEG_QUALITY, 70});
        _frame = std::move(jpeg);
        return {_frame.data(), _frame.size()};
    }

private:
    cv::VideoCapture _cap;
    int _width = TARGET_WIDTH, _height = TARGET_HEIGHT;
    bool _warmed = false;
    std::vector<uint8_t> _frame;
};

#endif  // __linux__

// ═══════════════════════════════════════════════════════════════════════════════
//  主函数（平台无关）
// ═══════════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "[camera-cpp] Starting..." << std::endl;

    void* ctx = init_dora_context_from_env();
    if (!ctx) {
        std::cerr << "[camera-cpp] Failed to init dora context" << std::endl;
        return 1;
    }
    std::cout << "[camera-cpp] Dora context initialized" << std::endl;

    Camera   cam;
    bool     capturing   = false;
    uint64_t tick_count  = 0;
    auto     last_report = std::chrono::steady_clock::now();
    uint64_t fps_counter = 0;

    while (true) {
        void* event = dora_next_event(ctx);
        if (!event) continue;

        int ev_type = read_dora_event_type((void*)event);

        if (ev_type == DoraEventType_Stop) {
            std::cout << "[camera-cpp] Stop received, exiting" << std::endl;
            free_dora_event(event);
            break;
        }

        if (ev_type == DoraEventType_Input) {
            char* id_ptr = nullptr;
            size_t id_len = 0;
            read_dora_input_id(event, &id_ptr, &id_len);
            std::string id(id_ptr, id_len);

            if (id == "control") {
                char* data_ptr = nullptr;
                size_t data_len = 0;
                read_dora_input_data(event, &data_ptr, &data_len);
                std::string cmd(data_ptr, data_len);

                if (cmd == "start") {
                    if (!cam.good() && !cam.open()) {
                        std::cerr << "[camera-cpp] Failed to open camera" << std::endl;
                        free_dora_event(event);
                        continue;
                    }
                    capturing = true;
                    std::cout << "[camera-cpp] capture started" << std::endl;
                } else if (cmd == "stop") {
                    capturing = false;
                    if (cam.good()) cam.close();
                    std::cout << "[camera-cpp] capture stopped" << std::endl;
                }
                free_dora_event(event);
                continue;
            }

            if (id == "tick" && cam.good() && capturing) {
                free_dora_event(event);

                if (!cam.wait_frame(2000)) continue;

                auto [data, len] = cam.read_frame();
                if (!data || len == 0) continue;

                dora_send_output(ctx, (char*)"image", 5, (char*)data, (size_t)len);

                tick_count++;
                fps_counter++;

                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_report).count();
                if (elapsed >= 1000) {
                    double fps = fps_counter * 1000.0 / elapsed;
                    std::cout << "[camera-cpp] " << cam.width() << "x" << cam.height()
                              << " " << (cam.is_mjpeg() ? "MJPEG" : "YUYV")
                              << " | " << (int)fps << " fps | "
                              << len / 1024 << " KB/frame" << std::endl;
                    fps_counter = 0;
                    last_report = std::chrono::steady_clock::now();
                }
                continue;
            }
        }
        free_dora_event(event);
    }

    if (cam.good()) cam.close();
    free_dora_context(ctx);
    std::cout << "[camera-cpp] Shutdown (" << tick_count << " frames)" << std::endl;
    return 0;
}
