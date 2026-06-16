/**
 * V4L2 Camera Benchmark (C++)
 *
 * 直接通过 V4L2 接口读取摄像头，测量每次 VIDIOC_DQBUF 调用的耗时，
 * 同时将指定帧保存为 JPEG 文件。
 *
 * Build:
 *   g++ -O2 -o bench_camera_v4l2 bench_camera_v4l2.cpp
 *
 * Usage:
 *   ./bench_camera_v4l2 [/dev/video0] [width] [height] [fps] [samples] [--save N] [--save-dir ./frames]
 *
 *   --save N    保存每第 N 帧 (0=不保存, 默认0)
 *
 * 输出:
 *   - VIDIOC_DQBUF syscall 延迟统计
 *   - 帧间隔统计和有效 FPS
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <linux/videodev2.h>

#define CLEAR(x) std::memset(&(x), 0, sizeof(x))

// ── Helper: high-resolution timestamp (microseconds) ─────────────

static double now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1'000'000.0 + ts.tv_nsec / 1'000.0;
}

// ── Helper: create directory ─────────────────────────────────────

static bool mkdir_p(const char* path) {
    std::string cmd = std::string("mkdir -p ") + path;
    return system(cmd.c_str()) == 0;
}

// ── V4L2 Camera ──────────────────────────────────────────────────

class V4L2Camera {
public:
    V4L2Camera(const char* device, int width, int height, int fps)
        : _fd(-1), _width(width), _height(height), _fps(fps), _buffers(nullptr), _n_bufs(0) {
        _fd = open(device, O_RDWR | O_NONBLOCK);
        if (_fd < 0) {
            std::fprintf(stderr, "[camera] ERROR: cannot open %s: %s\n", device, std::strerror(errno));
            return;
        }

        // Query capabilities
        v4l2_capability cap;
        CLEAR(cap);
        if (ioctl(_fd, VIDIOC_QUERYCAP, &cap) < 0) {
            std::fprintf(stderr, "[camera] ERROR: VIDIOC_QUERYCAP: %s\n", std::strerror(errno));
            close(_fd); _fd = -1; return;
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            std::fprintf(stderr, "[camera] ERROR: %s is not a video capture device\n", device);
            close(_fd); _fd = -1; return;
        }

        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
            std::fprintf(stderr, "[camera] ERROR: %s does not support streaming\n", device);
            close(_fd); _fd = -1; return;
        }

        // Try MJPEG first, fall back to YUYV
        _pixel_format = V4L2_PIX_FMT_MJPEG;
        if (!_try_format()) {
            std::fprintf(stderr, "[camera] MJPEG not supported, trying YUYV...\n");
            _pixel_format = V4L2_PIX_FMT_YUYV;
            if (!_try_format()) {
                std::fprintf(stderr, "[camera] ERROR: no supported format\n");
                close(_fd); _fd = -1; return;
            }
        }

        // Request buffers
        v4l2_requestbuffers req;
        CLEAR(req);
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(_fd, VIDIOC_REQBUFS, &req) < 0) {
            std::fprintf(stderr, "[camera] ERROR: VIDIOC_REQBUFS: %s\n", std::strerror(errno));
            close(_fd); _fd = -1; return;
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
                std::fprintf(stderr, "[camera] ERROR: VIDIOC_QUERYBUF: %s\n", std::strerror(errno));
                close(_fd); _fd = -1; return;
            }
            _buffers[i].length = buf.length;
            _buffers[i].start = mmap(nullptr, buf.length,
                PROT_READ | PROT_WRITE, MAP_SHARED, _fd, buf.m.offset);
            if (_buffers[i].start == MAP_FAILED) {
                std::fprintf(stderr, "[camera] ERROR: mmap: %s\n", std::strerror(errno));
                close(_fd); _fd = -1; return;
            }
        }

        // Queue all buffers
        for (unsigned i = 0; i < _n_bufs; i++) {
            v4l2_buffer buf;
            CLEAR(buf);
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(_fd, VIDIOC_QBUF, &buf) < 0) {
                std::fprintf(stderr, "[camera] ERROR: VIDIOC_QBUF: %s\n", std::strerror(errno));
                close(_fd); _fd = -1; return;
            }
        }

        // Start streaming
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_fd, VIDIOC_STREAMON, &type) < 0) {
            std::fprintf(stderr, "[camera] ERROR: VIDIOC_STREAMON: %s\n", std::strerror(errno));
            close(_fd); _fd = -1; return;
        }

        std::fprintf(stderr, "[camera] %s %dx%d @%dfps %s\n", device, width, height, fps,
            _pixel_format == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV");
    }

    ~V4L2Camera() { close_device(); }

    bool good() const { return _fd >= 0; }
    int fd() const { return _fd; }
    int width() const { return _width; }
    int height() const { return _height; }
    bool is_mjpeg() const { return _pixel_format == V4L2_PIX_FMT_MJPEG; }

    /**
     * Dequeue a frame. Returns (data_ptr, data_len) or (nullptr, 0) on timeout/error.
     * The returned data is a copy valid until the next call to read_frame().
     */
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

        // Copy frame data so we own it before requeuing the buffer
        _frame_buf.assign(src, src + len);
        ioctl(_fd, VIDIOC_QBUF, &buf);
        return {_frame_buf.data(), _frame_buf.size()};
    }

    /** Save a frame to a JPEG file (MJPEG frames are already JPEG). */
    bool save_frame(const uint8_t* data, size_t len, const char* path) {
        FILE* f = fopen(path, "wb");
        if (!f) {
            std::fprintf(stderr, "ERROR: cannot open %s for writing: %s\n", path, std::strerror(errno));
            return false;
        }
        size_t written = fwrite(data, 1, len, f);
        fclose(f);
        return written == len;
    }

private:
    struct Buffer {
        void* start;
        size_t length;
    };

    int _fd;
    int _width, _height, _fps;
    uint32_t _pixel_format;
    Buffer* _buffers;
    unsigned _n_bufs;
    std::vector<uint8_t> _frame_buf;

    bool _try_format() {
        v4l2_format fmt;
        CLEAR(fmt);
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = _width;
        fmt.fmt.pix.height = _height;
        fmt.fmt.pix.pixelformat = _pixel_format;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;

        if (ioctl(_fd, VIDIOC_S_FMT, &fmt) < 0) return false;
        _width = fmt.fmt.pix.width;
        _height = fmt.fmt.pix.height;
        return fmt.fmt.pix.pixelformat == _pixel_format;
    }

    void close_device() {
        if (_fd >= 0) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(_fd, VIDIOC_STREAMOFF, &type);
            for (unsigned i = 0; i < _n_bufs; i++) {
                if (_buffers[i].start != nullptr && _buffers[i].start != MAP_FAILED) {
                    munmap(_buffers[i].start, _buffers[i].length);
                }
            }
            delete[] _buffers; _buffers = nullptr;
            close(_fd); _fd = -1;
        }
    }
};

// ── Statistics ───────────────────────────────────────────────────

struct Stats {
    double avg, stddev, min, max, p50, p95, p99;
    int count;
};

static Stats compute_stats(std::vector<double>& data) {
    std::sort(data.begin(), data.end());
    int n = static_cast<int>(data.size());
    double sum = std::accumulate(data.begin(), data.end(), 0.0);
    double avg = n > 0 ? sum / n : 0.0;

    double variance = 0.0;
    for (double v : data) {
        double d = v - avg;
        variance += d * d;
    }
    double stddev = n > 0 ? std::sqrt(variance / n) : 0.0;

    auto pct = [&](double p) -> double {
        int idx = static_cast<int>(std::ceil(p / 100.0 * n)) - 1;
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        return data[idx];
    };

    return {
        avg,
        stddev,
        data.front(),
        data.back(),
        pct(50.0),
        pct(95.0),
        pct(99.0),
        n
    };
}

// ── Main ─────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::printf(
        "Usage: %s [device] [width] [height] [fps] [samples] [--save N] [--save-dir dir] [--warmup N]\n"
        "Defaults: /dev/video0 640 480 30 500\n"
        "  --save N    save every Nth frame (default 0 = don't save)\n"
        "Example:\n"
        "  %s /dev/video0 640 480 30 500 --save 50 --save-dir ./frames\n",
        prog, prog
    );
}

int main(int argc, char* argv[]) {
    // Parse arguments
    const char* device   = "/dev/video0";
    int width            = 640;
    int height           = 480;
    int fps              = 30;
    int samples          = 500;
    int save_n           = 0;
    int warmup           = 30;
    const char* save_dir = "./frames";

    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            save_n = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--save-dir") == 0 && i + 1 < argc) {
            save_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = std::atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            switch (positional++) {
                case 0: device  = argv[i]; break;
                case 1: width   = std::atoi(argv[i]); break;
                case 2: height  = std::atoi(argv[i]); break;
                case 3: fps     = std::atoi(argv[i]); break;
                case 4: samples = std::atoi(argv[i]); break;
            }
        }
    }

    std::printf("V4L2 Camera Benchmark\n");
    std::printf("  Device:   %s\n", device);
    std::printf("  Size:     %dx%d\n", width, height);
    std::printf("  Target:   %d fps\n", fps);
    std::printf("  Samples:  %d\n", samples);
    std::printf("  Warmup:   %d\n", warmup);
    std::printf("  Save:     %d frames\n", save_n);
    std::printf("\n");

    // Setup save directory
    if (save_n > 0) {
        mkdir_p(save_dir);
        std::printf("Saving %d frames to %s/\n", save_n, save_dir);
    }

    // Open camera
    V4L2Camera cam(device, width, height, fps);
    if (!cam.good()) {
        std::fprintf(stderr, "FATAL: camera init failed\n");
        return 1;
    }

    std::printf("Camera opened: %dx%d @%dfps %s\n",
        cam.width(), cam.height(), fps, cam.is_mjpeg() ? "MJPEG" : "YUYV");

    int cam_fd = cam.fd();
    const int poll_timeout_ms = 2000;  // 2s timeout per frame

    // Warmup
    std::printf("\nWarming up (%d frames)...\n", warmup);
    for (int i = 0; i < warmup; i++) {
        // Wait for frame ready
        struct pollfd pfd;
        pfd.fd = cam_fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, poll_timeout_ms);
        if (ret <= 0) {
            std::fprintf(stderr, "WARNING: warmup frame %d poll timeout\n", i);
            continue;
        }
        auto frame = cam.read_frame();
        if (frame.first == nullptr) {
            std::fprintf(stderr, "WARNING: warmup frame %d dequeue failed\n", i);
        }
    }

    // Benchmark
    std::printf("Benchmarking (%d frames)...\n", samples);
    if (save_n > 0) {
        std::printf("  saving first %d frames to %s/\n", save_n, save_dir);
    }

    std::vector<double> dq_latencies_us;   // DQBUF syscall latency only
    std::vector<double> frame_intervals_us; // total: poll wait + DQBUF
    dq_latencies_us.reserve(samples);
    frame_intervals_us.reserve(samples);

    char path_buf[512];
    double last_frame_ts = 0.0;

    for (int i = 0; i < samples; i++) {
        // Wait for a frame to be ready
        struct pollfd pfd;
        pfd.fd = cam_fd;
        pfd.events = POLLIN;
        double poll_t0 = now_us();
        int ret = poll(&pfd, 1, poll_timeout_ms);
        double poll_t1 = now_us();

        if (ret <= 0) {
            std::fprintf(stderr, "WARNING: sample %d poll timeout (%d ms), skipping\n",
                i, poll_timeout_ms);
            continue;
        }

        // Dequeue (frame is ready, no EAGAIN expected)
        double dq_t0 = now_us();
        auto frame = cam.read_frame();
        const uint8_t* data = frame.first;
        size_t len = frame.second;
        double dq_t1 = now_us();

        if (data == nullptr || len == 0) {
            std::fprintf(stderr, "WARNING: sample %d dequeue failed after poll, skipping\n", i);
            continue;
        }

        double dq_elapsed_us = dq_t1 - dq_t0;
        dq_latencies_us.push_back(dq_elapsed_us);

        // Frame interval: time between successive successful dequeues
        if (i > 0 || last_frame_ts > 0.0) {
            double interval_us = dq_t1 - last_frame_ts;
            frame_intervals_us.push_back(interval_us);
        }
        last_frame_ts = dq_t1;

        // Save every Nth frame (after timing, so it doesn't affect latency measurement)
        if (save_n > 0 && i % save_n == 0) {
            std::snprintf(path_buf, sizeof(path_buf), "%s/frame_%04d.jpg", save_dir, i + 1);
            cam.save_frame(data, len, path_buf);
        }
    }

    if (dq_latencies_us.empty()) {
        std::fprintf(stderr, "ERROR: No valid frames captured\n");
        return 1;
    }

    // Statistics
    Stats dq_stats = compute_stats(dq_latencies_us);

    std::printf("\n");
    std::printf("==============================================================\n");
    std::printf("  V4L2 Camera Benchmark Results\n");
    std::printf("==============================================================\n");

    std::printf("\n  ── VIDIOC_DQBUF syscall latency ──\n");
    std::printf("  Samples:       %d\n", dq_stats.count);
    std::printf("  Avg:           %8.1f µs\n", dq_stats.avg);
    std::printf("  Min:           %8.1f µs\n", dq_stats.min);
    std::printf("  Max:           %8.1f µs\n", dq_stats.max);
    std::printf("  P50 (median):  %8.1f µs\n", dq_stats.p50);
    std::printf("  P95:           %8.1f µs\n", dq_stats.p95);
    std::printf("  P99:           %8.1f µs\n", dq_stats.p99);
    std::printf("  Stddev:        %8.1f µs\n", dq_stats.stddev);

    if (!frame_intervals_us.empty()) {
        Stats fi_stats = compute_stats(frame_intervals_us);
        std::printf("\n  ── Frame interval (poll + DQBUF) ──\n");
        std::printf("  Samples:       %d\n", fi_stats.count);
        std::printf("  Avg:           %8.1f µs\n", fi_stats.avg);
        std::printf("  Min:           %8.1f µs\n", fi_stats.min);
        std::printf("  Max:           %8.1f µs\n", fi_stats.max);
        std::printf("  P50 (median):  %8.1f µs\n", fi_stats.p50);
        std::printf("  P95:           %8.1f µs\n", fi_stats.p95);
        std::printf("  P99:           %8.1f µs\n", fi_stats.p99);
        std::printf("  Effective FPS: %8.1f\n", 1'000'000.0 / fi_stats.avg);
    }

    std::printf("==============================================================\n");

    if (save_n > 0) {
        int saved = std::min(save_n, dq_stats.count);
        std::printf("\nSaved %d frames to %s/\n", saved, save_dir);
    }

    return 0;
}