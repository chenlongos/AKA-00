/// dora C++ 摄像头节点 —— V4L2 直接采集 MJPEG 帧，原样输出
/// 摄像头在 stop 时彻底释放设备，start 时重新打开
///
/// 参考: tests/bench_camera_v4l2.cpp 的 V4L2Camera 实现
#include "node_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CLEAR(x) std::memset(&(x), 0, sizeof(x))

constexpr int TARGET_WIDTH  = 640;
constexpr int TARGET_HEIGHT = 480;
constexpr int TARGET_FPS    = 30;

// ── V4L2 摄像头封装 (同 bench_camera_v4l2.cpp) ───────────────────────────────

class V4L2Camera {
public:
    V4L2Camera() {}

    bool open(const char* device = "/dev/video0") {
        _device = device;
        _fd = ::open(device, O_RDWR | O_NONBLOCK);
        if (_fd < 0) {
            std::fprintf(stderr, "[camera-cpp] ERROR: cannot open %s: %s\n", device, std::strerror(errno));
            return false;
        }

        v4l2_capability cap;
        CLEAR(cap);
        if (ioctl(_fd, VIDIOC_QUERYCAP, &cap) < 0) {
            std::fprintf(stderr, "[camera-cpp] ERROR: VIDIOC_QUERYCAP: %s\n", std::strerror(errno));
            close(); return false;
        }
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            std::fprintf(stderr, "[camera-cpp] ERROR: %s is not a video capture device\n", device);
            close(); return false;
        }
        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
            std::fprintf(stderr, "[camera-cpp] ERROR: %s does not support streaming\n", device);
            close(); return false;
        }

        // 优先 MJPEG
        _pixel_format = V4L2_PIX_FMT_MJPEG;
        if (!_try_format(_width, _height)) {
            std::fprintf(stderr, "[camera-cpp] MJPEG not supported, trying YUYV...\n");
            _pixel_format = V4L2_PIX_FMT_YUYV;
            if (!_try_format(_width, _height)) {
                std::fprintf(stderr, "[camera-cpp] ERROR: no supported format\n");
                close(); return false;
            }
        }

        // 申请 mmap buffers
        v4l2_requestbuffers req;
        CLEAR(req);
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(_fd, VIDIOC_REQBUFS, &req) < 0) {
            std::fprintf(stderr, "[camera-cpp] ERROR: VIDIOC_REQBUFS: %s\n", std::strerror(errno));
            close(); return false;
        }
        _n_bufs = req.count;
        _buffers = new Buffer[_n_bufs];

        for (unsigned i = 0; i < _n_bufs; i++) {
            v4l2_buffer buf;
            CLEAR(buf);
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(_fd, VIDIOC_QUERYBUF, &buf) < 0) {
                std::fprintf(stderr, "[camera-cpp] ERROR: VIDIOC_QUERYBUF: %s\n", std::strerror(errno));
                close(); return false;
            }
            _buffers[i].length = buf.length;
            _buffers[i].start = mmap(nullptr, buf.length,
                PROT_READ | PROT_WRITE, MAP_SHARED, _fd, buf.m.offset);
            if (_buffers[i].start == MAP_FAILED) {
                std::fprintf(stderr, "[camera-cpp] ERROR: mmap: %s\n", std::strerror(errno));
                close(); return false;
            }
        }

        for (unsigned i = 0; i < _n_bufs; i++) {
            v4l2_buffer buf;
            CLEAR(buf);
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(_fd, VIDIOC_QBUF, &buf) < 0) {
                std::fprintf(stderr, "[camera-cpp] ERROR: VIDIOC_QBUF: %s\n", std::strerror(errno));
                close(); return false;
            }
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_fd, VIDIOC_STREAMON, &type) < 0) {
            std::fprintf(stderr, "[camera-cpp] ERROR: VIDIOC_STREAMON: %s\n", std::strerror(errno));
            close(); return false;
        }

        const char* fmt = (_pixel_format == V4L2_PIX_FMT_MJPEG) ? "MJPEG" : "YUYV";
        std::cout << "[camera-cpp] opened " << _width << "x" << _height
                  << " " << fmt << std::endl;
        return true;
    }

    void close() {
        if (_fd >= 0) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(_fd, VIDIOC_STREAMOFF, &type);
            for (unsigned i = 0; i < _n_bufs; i++) {
                if (_buffers[i].start && _buffers[i].start != MAP_FAILED)
                    munmap(_buffers[i].start, _buffers[i].length);
            }
            delete[] _buffers; _buffers = nullptr;
            ::close(_fd); _fd = -1;
        }
        std::cout << "[camera-cpp] closed (device released)" << std::endl;
    }

    bool good() const { return _fd >= 0; }
    int fd() const { return _fd; }
    int width() const { return _width; }
    int height() const { return _height; }
    bool is_mjpeg() const { return _pixel_format == V4L2_PIX_FMT_MJPEG; }

    /// Dequeue 一帧，拷贝后立即 QBUF 归还内核
    std::pair<const uint8_t*, size_t> read_frame() {
        v4l2_buffer buf;
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return {nullptr, 0};
            return {nullptr, 0};
        }

        const uint8_t* src = static_cast<const uint8_t*>(_buffers[buf.index].start);
        size_t len = buf.bytesused;
        _frame_buf.assign(src, src + len);
        ioctl(_fd, VIDIOC_QBUF, &buf);
        return {_frame_buf.data(), _frame_buf.size()};
    }

private:
    struct Buffer { void* start; size_t length; };

    const char* _device = "/dev/video0";
    int         _fd = -1;
    int         _width = TARGET_WIDTH;
    int         _height = TARGET_HEIGHT;
    uint32_t    _pixel_format = 0;
    Buffer*     _buffers = nullptr;
    unsigned    _n_bufs = 0;
    std::vector<uint8_t> _frame_buf;

    bool _try_format(int& w, int& h) {
        v4l2_format fmt;
        CLEAR(fmt);
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = w;
        fmt.fmt.pix.height      = h;
        fmt.fmt.pix.pixelformat = _pixel_format;
        fmt.fmt.pix.field       = V4L2_FIELD_ANY;
        if (ioctl(_fd, VIDIOC_S_FMT, &fmt) < 0) return false;
        w = fmt.fmt.pix.width;
        h = fmt.fmt.pix.height;
        return fmt.fmt.pix.pixelformat == _pixel_format;
    }
};


// ── 主函数 ───────────────────────────────────────────────────────────────────

int main() {
    std::cout << "[camera-cpp] Starting..." << std::endl;

    void* ctx = init_dora_context_from_env();
    if (!ctx) {
        std::cerr << "[camera-cpp] Failed to init dora context" << std::endl;
        return 1;
    }
    std::cout << "[camera-cpp] Dora context initialized" << std::endl;

    V4L2Camera cam;
    bool       capturing  = false;
    uint64_t   tick_count = 0;
    auto       last_report = std::chrono::steady_clock::now();
    uint64_t   fps_counter = 0;
    const int  poll_timeout_ms = 2000;

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

                struct pollfd pfd;
                pfd.fd = cam.fd();
                pfd.events = POLLIN;
                if (poll(&pfd, 1, poll_timeout_ms) <= 0) continue;

                auto [data, len] = cam.read_frame();
                if (!data || len == 0) continue;

                // MJPEG 帧原样输出
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
