/**
 * LAN Video Streaming Benchmark (C++)
 *
 * 从 V4L2 摄像头采集 MJPEG 帧，通过内置 HTTP 服务器将视频流
 * 传输到局域网浏览器，实现第一人称视角（FPV）查看，并实时测量
 * 流传输性能指标。
 *
 * 特性:
 *   - 内置轻量 HTTP 服务器 (默认端口 8080)
 *   - MJPEG 流端点 /stream （浏览器原生支持）
 *   - 沉浸式 FPV 网页界面，带性能 HUD
 *   - 实时统计 API /api/stats
 *   - 延迟、FPS、带宽 测量
 *
 * Build:
 *   g++ -O2 -std=c++17 -o bench_lan_streaming bench_lan_streaming.cpp
 *
 * Usage:
 *   ./bench_lan_streaming [/dev/video0] [port] [width] [height] [fps]
 *
 *   Defaults: /dev/video0 8080 640 480 30
 *
 * 然后浏览器打开: http://<树莓派IP>:8080
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
#include <deque>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <sstream>
#include <memory>

#include <fcntl.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#include <sys/uio.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <linux/videodev2.h>

#define CLEAR(x) std::memset(&(x), 0, sizeof(x))

// ====================================================================
//  High-precision timestamp (microseconds)
// ====================================================================

static double now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1'000'000.0 + ts.tv_nsec / 1'000.0;
}

static std::string format_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    std::snprintf(buf + 8, sizeof(buf) - 8, ".%03lld", (long long)ms.count());
    return buf;
}

// ====================================================================
//  Ring buffer for sliding-window stats
// ====================================================================

template<typename T, size_t N>
class RingBuffer {
    T _buf[N];
    size_t _head = 0;
    size_t _count = 0;
public:
    void push(T v) {
        _buf[_head] = v;
        _head = (_head + 1) % N;
        if (_count < N) _count++;
    }
    size_t size() const { return _count; }
    T operator[](size_t i) const { return _buf[(_head - _count + i + N) % N]; }

    double avg() const {
        if (_count == 0) return 0;
        double s = 0;
        for (size_t i = 0; i < _count; i++) s += (*this)[i];
        return s / _count;
    }
    T max() const {
        if (_count == 0) return 0;
        T m = (*this)[0];
        for (size_t i = 1; i < _count; i++) m = std::max(m, (*this)[i]);
        return m;
    }
    T min() const {
        if (_count == 0) return 0;
        T m = (*this)[0];
        for (size_t i = 1; i < _count; i++) m = std::min(m, (*this)[i]);
        return m;
    }
};

// ====================================================================
//  Streaming statistics (thread-safe)
// ====================================================================

struct StreamStats {
    std::mutex mtx;

    // Per-frame timing
    RingBuffer<double, 300> capture_latency_us;   // DQBUF syscall time
    RingBuffer<double, 300> frame_interval_us;     // time between frames
    RingBuffer<double, 300> send_latency_us;       // write() to socket time
    RingBuffer<size_t, 300> frame_bytes;           // bytes per frame

    // Counters
    uint64_t total_frames = 0;
    uint64_t total_bytes = 0;
    uint64_t dropped_frames = 0;
    int active_clients = 0;
    double start_time_us = 0;
    double last_frame_ts_us = 0;
    std::string server_ip;

    double elapsed_sec() const {
        if (start_time_us == 0) return 0;
        return (now_us() - start_time_us) / 1'000'000.0;
    }

    // Snapshot for JSON output
    std::string to_json() {
        std::lock_guard<std::mutex> lock(mtx);
        double elapsed = elapsed_sec();
        double fps = frame_interval_us.size() > 0
            ? 1'000'000.0 / frame_interval_us.avg() : 0.0;
        double bandwidth_kbps = elapsed > 0
            ? (total_bytes * 8.0 / 1000.0) / elapsed : 0.0;

        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "{"
            "\"fps\":%.1f,"
            "\"total_frames\":%lu,"
            "\"total_mb\":%.2f,"
            "\"bandwidth_kbps\":%.1f,"
            "\"active_clients\":%d,"
            "\"dropped_frames\":%lu,"
            "\"elapsed_sec\":%.1f,"
            "\"capture_avg_us\":%.1f,"
            "\"capture_max_us\":%.1f,"
            "\"frame_interval_avg_us\":%.1f,"
            "\"frame_interval_max_us\":%.1f,"
            "\"send_avg_us\":%.1f,"
            "\"send_max_us\":%.1f,"
            "\"frame_avg_kb\":%.1f,"
            "\"frame_max_kb\":%.1f"
            "}",
            fps,
            (unsigned long)total_frames,
            total_bytes / (1024.0 * 1024.0),
            bandwidth_kbps,
            active_clients,
            (unsigned long)dropped_frames,
            elapsed,
            capture_latency_us.avg(),
            capture_latency_us.max(),
            frame_interval_us.avg(),
            frame_interval_us.max(),
            send_latency_us.avg(),
            send_latency_us.max(),
            frame_bytes.avg() / 1024.0,
            frame_bytes.max() / 1024.0
        );
        return buf;
    }
};

// ====================================================================
//  Thread-safe frame buffer (single producer, multiple consumers)
//  Uses shared_ptr to avoid copying frame data — consumers hold a
//  reference while the producer can publish new frames independently.
// ====================================================================

class FrameBuffer {
public:
    void publish(std::shared_ptr<const std::vector<uint8_t>> frame) {
        std::lock_guard<std::mutex> lock(_mtx);
        _buffer = std::move(frame);
        _seq++;
        _cv.notify_all();
    }

    // Returns shared_ptr (cheap reference-count bump) and sequence number.
    // No data copy — consumers read from the same underlying vector.
    std::pair<std::shared_ptr<const std::vector<uint8_t>>, uint64_t>
    get_frame(uint64_t last_seq) {
        std::unique_lock<std::mutex> lock(_mtx);
        if (last_seq == _seq) {
            _cv.wait_for(lock, std::chrono::milliseconds(100));
        }
        return {_buffer, _seq};
    }

    bool has_frame() const {
        std::lock_guard<std::mutex> lock(_mtx);
        return _seq > 0 && _buffer != nullptr;
    }

private:
    mutable std::mutex _mtx;
    std::condition_variable _cv;
    std::shared_ptr<const std::vector<uint8_t>> _buffer;
    uint64_t _seq = 0;
};

// ====================================================================
//  V4L2 Camera (same as bench_camera_v4l2.cpp)
// ====================================================================

class V4L2Camera {
public:
    V4L2Camera(const char* device, int width, int height, int fps)
        : _fd(-1), _width(width), _height(height), _fps(fps), _buffers(nullptr), _n_bufs(0) {
        _fd = open(device, O_RDWR | O_NONBLOCK);
        if (_fd < 0) {
            std::fprintf(stderr, "[camera] ERROR: cannot open %s: %s\n", device, std::strerror(errno));
            return;
        }

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

        _pixel_format = V4L2_PIX_FMT_MJPEG;
        if (!_try_format()) {
            std::fprintf(stderr, "[camera] MJPEG not supported, trying YUYV...\n");
            _pixel_format = V4L2_PIX_FMT_YUYV;
            if (!_try_format()) {
                std::fprintf(stderr, "[camera] ERROR: no supported format\n");
                close(_fd); _fd = -1; return;
            }
        }

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

    // Returns a shared_ptr to the frame data.  The data is copied out of
    // the mmap buffer so that QBUF can return the buffer to the kernel
    // immediately — this one copy is unavoidable.
    std::shared_ptr<std::vector<uint8_t>> read_frame() {
        v4l2_buffer buf;
        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) return nullptr;
            return nullptr;
        }

        const uint8_t* src = static_cast<const uint8_t*>(_buffers[buf.index].start);
        size_t len = buf.bytesused;
        auto frame = std::make_shared<std::vector<uint8_t>>(src, src + len);
        ioctl(_fd, VIDIOC_QBUF, &buf);
        return frame;
    }

private:
    struct Buffer { void* start; size_t length; };

    int _fd, _width, _height, _fps;
    uint32_t _pixel_format;
    Buffer* _buffers;
    unsigned _n_bufs;

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
                if (_buffers[i].start != nullptr && _buffers[i].start != MAP_FAILED)
                    munmap(_buffers[i].start, _buffers[i].length);
            }
            delete[] _buffers; _buffers = nullptr;
            close(_fd); _fd = -1;
        }
    }
};

// ====================================================================
//  Minimal HTTP/1.1 Server
// ====================================================================

class HttpServer {
public:
    HttpServer(int port, StreamStats& stats, FrameBuffer& frame_buf)
        : _port(port), _stats(stats), _frame_buf(frame_buf), _listen_fd(-1), _running(false) {}

    ~HttpServer() { stop(); }

    bool start() {
        _listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (_listen_fd < 0) {
            std::fprintf(stderr, "[http] socket() failed: %s\n", std::strerror(errno));
            return false;
        }

        int opt = 1;
        setsockopt(_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(_listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        struct sockaddr_in addr;
        CLEAR(addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(_port);

        if (bind(_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::fprintf(stderr, "[http] bind() failed: %s\n", std::strerror(errno));
            close(_listen_fd); _listen_fd = -1;
            return false;
        }

        if (listen(_listen_fd, 10) < 0) {
            std::fprintf(stderr, "[http] listen() failed: %s\n", std::strerror(errno));
            close(_listen_fd); _listen_fd = -1;
            return false;
        }

        // Make listen_fd non-blocking
        int flags = fcntl(_listen_fd, F_GETFL, 0);
        fcntl(_listen_fd, F_SETFL, flags | O_NONBLOCK);

        _running = true;
        _thread = std::thread(&HttpServer::_server_loop, this);

        // Detect server IP
        _detect_ip();

        std::fprintf(stderr, "[http] listening on http://%s:%d\n",
            _stats.server_ip.c_str(), _port);
        return true;
    }

    void stop() {
        _running = false;
        if (_thread.joinable()) _thread.join();
        if (_listen_fd >= 0) { close(_listen_fd); _listen_fd = -1; }
    }

private:
    int _port;
    int _listen_fd;
    StreamStats& _stats;
    FrameBuffer& _frame_buf;
    std::atomic<bool> _running;
    std::thread _thread;

    struct Client {
        int fd;
        std::string recv_buf;
        bool is_stream = false;     // MJPEG stream client
        uint64_t last_frame_seq = 0;
    };

    void _detect_ip() {
        // Try to get the primary network interface IP
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { _stats.server_ip = "127.0.0.1"; return; }

        struct sockaddr_in addr;
        CLEAR(addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(80);
        inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            getsockname(fd, (struct sockaddr*)&local, &len);
            _stats.server_ip = inet_ntoa(local.sin_addr);
        } else {
            _stats.server_ip = "127.0.0.1";
        }
        close(fd);
    }

    void _server_loop() {
        std::vector<Client> clients;

#ifdef __linux__
        // ── Linux: epoll (O(1) event dispatch) ──────────────────────
        int epfd = epoll_create1(0);
        if (epfd < 0) {
            std::fprintf(stderr, "[http] epoll_create1 failed: %s\n", std::strerror(errno));
            return;
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = _listen_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, _listen_fd, &ev);

        struct epoll_event events[64];

        while (_running) {
            int nfds = epoll_wait(epfd, events, 64, 100);
            if (nfds < 0) {
                if (errno == EINTR) continue;
                break;
            }

            for (int i = 0; i < nfds; i++) {
                int fd = events[i].data.fd;

                if (fd == _listen_fd) {
                    _accept_client_epoll(clients, epfd);
                    continue;
                }

                Client* c = _find_client(clients, fd);
                if (!c) continue;

                uint32_t revents = events[i].events;

                if (revents & (EPOLLERR | EPOLLHUP)) {
                    _close_client_epoll(clients, epfd, fd);
                    continue;
                }

                if (revents & EPOLLIN) {
                    if (!_handle_read(*c)) {
                        _close_client_epoll(clients, epfd, fd);
                        continue;
                    }
                }
            }

            _push_stream_frames(clients);
        }

        for (auto& c : clients) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, c.fd, nullptr);
            close(c.fd);
        }
        close(epfd);

#else
        // ── Fallback: poll() — portable but O(n) per iteration ──────
        std::vector<struct pollfd> fds;

        while (_running) {
            fds.clear();
            fds.push_back({_listen_fd, POLLIN, 0});
            for (auto& c : clients) {
                fds.push_back({c.fd, POLLIN, 0});
            }

            int ret = poll(fds.data(), fds.size(), 100);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (fds[0].revents & POLLIN) {
                _accept_client_poll(clients);
            }

            for (int ci = (int)clients.size() - 1; ci >= 0; ci--) {
                int fi = ci + 1;
                if (fi >= (int)fds.size()) continue;

                if (fds[fi].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    _close_client_poll(clients, ci);
                    continue;
                }

                if (fds[fi].revents & POLLIN) {
                    if (!_handle_read(clients[ci])) {
                        _close_client_poll(clients, ci);
                        continue;
                    }
                }
            }

            _push_stream_frames(clients);
        }

        for (auto& c : clients) close(c.fd);
#endif
    }

    // ── Client management: epoll (Linux) ─────────────────────────────

    void _accept_client_epoll(std::vector<Client>& clients, int epfd) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int fd = accept(_listen_fd, (struct sockaddr*)&addr, &addr_len);
        if (fd < 0) return;

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        // Larger send buffer to reduce EAGAIN under load
        int sndbuf = 256 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        // Register with epoll (level-triggered)
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

        clients.push_back({fd, "", false, 0});
        _stats.active_clients = (int)clients.size();
    }

    void _close_client_epoll(std::vector<Client>& clients, int epfd, int fd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            if (it->fd == fd) { clients.erase(it); break; }
        }
        _stats.active_clients = (int)clients.size();
    }

    // ── Client management: poll() fallback ───────────────────────────

    void _accept_client_poll(std::vector<Client>& clients) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int fd = accept(_listen_fd, (struct sockaddr*)&addr, &addr_len);
        if (fd < 0) return;

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        int sndbuf = 256 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        clients.push_back({fd, "", false, 0});
        _stats.active_clients = (int)clients.size();
    }

    void _close_client_poll(std::vector<Client>& clients, int idx) {
        if (idx < 0 || idx >= (int)clients.size()) return;
        close(clients[idx].fd);
        clients.erase(clients.begin() + idx);
        _stats.active_clients = (int)clients.size();
    }

    bool _handle_read(Client& c) {
        char buf[8192];
        ssize_t n = recv(c.fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return n == 0 ? false : (errno == EAGAIN || errno == EWOULDBLOCK);

        buf[n] = '\0';
        c.recv_buf += buf;

        // Check for complete HTTP request
        size_t hdr_end = c.recv_buf.find("\r\n\r\n");
        if (hdr_end == std::string::npos) {
            if (c.recv_buf.size() > 65536) return false; // too large
            return true;
        }

        std::string request = c.recv_buf.substr(0, hdr_end);
        c.recv_buf.clear();

        // Parse request line
        size_t nl = request.find("\r\n");
        std::string req_line = request.substr(0, nl);
        std::string path = "/";
        size_t s1 = req_line.find(' ');
        size_t s2 = req_line.rfind(' ');
        if (s1 != std::string::npos && s2 != std::string::npos && s2 > s1)
            path = req_line.substr(s1 + 1, s2 - s1 - 1);

        // Route
        if (path == "/stream") {
            // MJPEG stream — send headers immediately
            return _start_stream(c);
        } else if (path == "/api/stats") {
            return _send_stats(c);
        } else {
            return _send_page(c);
        }
    }

    bool _handle_write(Client& c) {
        // Non-stream writes are handled inline in _send_response / _start_stream
        return true;
    }

    bool _send_response(int fd, const std::string& content, const char* content_type) {
        char header[512];
        std::snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "\r\n",
            content_type, content.size());

        // Non-blocking send
        std::string full = std::string(header) + content;
        size_t sent = 0;
        while (sent < full.size()) {
            ssize_t n = send(fd, full.data() + sent, full.size() - sent, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd = {fd, POLLOUT, 0};
                    poll(&pfd, 1, 1000);
                    continue;
                }
                return false;
            }
            sent += n;
        }
        return true;
    }

    bool _start_stream(Client& c) {
        const char* header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Pragma: no-cache\r\n"
            "Expires: 0\r\n"
            "\r\n";

        size_t sent = 0;
        size_t len = std::strlen(header);
        while (sent < len) {
            ssize_t n = send(c.fd, header + sent, len - sent, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd = {c.fd, POLLOUT, 0};
                    poll(&pfd, 1, 1000);
                    continue;
                }
                return false;
            }
            sent += n;
        }

        c.is_stream = true;
        // std::fprintf(stderr, "[http] MJPEG stream started\n");
        return true;
    }

    void _push_stream_frames(std::vector<Client>& clients) {
        if (!_frame_buf.has_frame()) return;

        auto result = _frame_buf.get_frame(0);
        auto& frame_ptr = result.first;
        uint64_t seq = result.second;
        if (!frame_ptr || frame_ptr->empty()) return;

        const uint8_t* data = frame_ptr->data();
        size_t len = frame_ptr->size();

        // Pre-build MJPEG boundary header
        char boundary_hdr[256];
        int hdr_len = std::snprintf(boundary_hdr, sizeof(boundary_hdr),
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", len);

        // Single writev() replaces three send() syscalls
        struct iovec iov[3] = {
            {boundary_hdr, (size_t)hdr_len},
            {(void*)data, len},
            {(void*)"\r\n", 2}
        };
        size_t total = hdr_len + len + 2;

        for (auto& c : clients) {
            if (!c.is_stream) continue;
            if (c.last_frame_seq == seq) continue;

            double send_t0 = now_us();

            // TCP_CORK: tell the kernel to hold packets until we uncork,
            // so the entire frame goes out as one TCP segment
            _cork(c.fd);
            int rc = _send_iov_nonblock(c.fd, iov, 3, total);
            _uncork(c.fd);

            double send_t1 = now_us();

            if (rc == 0) {
                // Success — record timing
                c.last_frame_seq = seq;
                _stats.send_latency_us.push(send_t1 - send_t0);
            }
            // rc == -1: EAGAIN — socket buffer full, silently skip frame
            // rc == -2: real error — client will be cleaned by next epoll cycle
        }
    }

    // ── TCP optimisations ───────────────────────────────────────────

    void _cork(int fd) {
#ifdef __linux__
        int state = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_CORK, &state, sizeof(state));
#elif defined(__APPLE__)
        int state = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NOPUSH, &state, sizeof(state));
#else
        (void)fd;
#endif
    }
    void _uncork(int fd) {
#ifdef __linux__
        int state = 0;
        setsockopt(fd, IPPROTO_TCP, TCP_CORK, &state, sizeof(state));
#elif defined(__APPLE__)
        int state = 0;
        setsockopt(fd, IPPROTO_TCP, TCP_NOPUSH, &state, sizeof(state));
#else
        (void)fd;
#endif
    }

    // Send an iovec array completely, non-blocking.
    // Returns 0 on success, -1 on EAGAIN (socket buffer full → skip frame),
    // -2 on real error.
    int _send_iov_nonblock(int fd, struct iovec* iov, int iovcnt, size_t total) {
        struct iovec local_iov[3];
        std::memcpy(local_iov, iov, iovcnt * sizeof(struct iovec));
        struct iovec* iov_ptr = local_iov;
        int iov_remain = iovcnt;

        size_t written = 0;
        while (written < total) {
            ssize_t n = writev(fd, iov_ptr, iov_remain);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return -1;   // socket buffer full — caller should skip this frame
                return -2;       // real error — caller should close client
            }
            written += n;
            // Advance iovec pointers past what was written
            while (n > 0 && iov_remain > 0) {
                if ((size_t)n >= iov_ptr->iov_len) {
                    n -= iov_ptr->iov_len;
                    iov_ptr++;
                    iov_remain--;
                } else {
                    iov_ptr->iov_base = (char*)iov_ptr->iov_base + n;
                    iov_ptr->iov_len -= n;
                    n = 0;
                }
            }
        }
        return 0;
    }

    // Find a client by fd (linear scan — fine for bench-scale client counts)
    Client* _find_client(std::vector<Client>& clients, int fd) {
        for (auto& c : clients) {
            if (c.fd == fd) return &c;
        }
        return nullptr;
    }

    bool _send_stats(Client& c) {
        std::string json = _stats.to_json();
        return _send_response(c.fd, json, "application/json");
    }

    bool _send_page(Client& c) {
        std::string html = _build_html();
        return _send_response(c.fd, html, "text/html; charset=utf-8");
    }

    // ================================================================
    //  FPV HTML Page (inline, no external dependencies)
    // ================================================================

    std::string _build_html() {
        return R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#000000">
<title>FPV - LAN Streaming Benchmark</title>
<style>
  * { margin:0; padding:0; box-sizing:border-box; }
  html, body { width:100%; height:100%; overflow:hidden; background:#000;
               font-family: 'SF Mono', 'Menlo', 'Consolas', monospace; }

  /* Fullscreen video layer */
  #video-container {
    position: fixed; top:0; left:0; width:100%; height:100%;
    display:flex; align-items:center; justify-content:center;
    background:#000; z-index:1;
  }
  #video-container img {
    width:100%; height:100%;
    object-fit: contain;
    image-rendering: auto;
  }

  /* HUD overlay */
  #hud {
    position: fixed; top:0; left:0; width:100%; z-index:10;
    pointer-events: none;
    padding: 12px 16px;
    transition: opacity 0.3s;
  }
  #hud.hidden { opacity: 0; }

  /* Stats panel */
  #stats {
    display: flex; flex-wrap: wrap; gap: 8px 16px;
    background: rgba(0,0,0,0.55);
    backdrop-filter: blur(12px);
    -webkit-backdrop-filter: blur(12px);
    border-radius: 12px;
    padding: 10px 16px;
    color: #0f0;
    font-size: 13px;
    line-height: 1.6;
    max-width: 420px;
  }
  #stats .label { color: rgba(255,255,255,0.5); font-size: 10px; text-transform: uppercase; letter-spacing: 0.5px; }
  #stats .value { font-size: 18px; font-weight: 700; }
  #stats .warn  { color: #f80; }
  #stats .bad   { color: #f00; }

  /* Connection indicator */
  #conn-dot {
    display: inline-block; width:8px; height:8px; border-radius:50%;
    background:#0f0; margin-right:4px; vertical-align:middle;
    animation: pulse 1s infinite;
  }
  #conn-dot.dead { background:#f00; animation:none; }
  @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.3} }

  /* Tap hint */
  #tap-hint {
    position: fixed; bottom: 40px; left: 50%; transform: translateX(-50%);
    color: rgba(255,255,255,0.5); font-size: 14px; z-index:20;
    transition: opacity 0.8s; pointer-events: none;
  }

  /* Performance strip (bottom bar) */
  #perf-bar {
    position: fixed; bottom: 0; left: 0; width: 100%; height: 3px;
    z-index: 10; background: rgba(255,255,255,0.1);
  }
  #perf-fill {
    height: 100%; background: #0f0;
    transition: width 0.3s, background 0.3s;
  }
</style>
</head>
<body>

<div id="video-container">
  <img id="mjpeg" src="/stream" alt="FPV Stream"
       onerror="onStreamError()"
       onload="onStreamLoad()" />
</div>

<div id="hud">
  <div id="stats">
    <div style="min-width:60px;">
      <div class="label">FPS</div>
      <div class="value" id="s-fps">--</div>
    </div>
    <div style="min-width:70px;">
      <div class="label">延迟 (ms)</div>
      <div class="value" id="s-lat">--</div>
    </div>
    <div style="min-width:90px;">
      <div class="label">带宽 (kbps)</div>
      <div class="value" id="s-bw">--</div>
    </div>
    <div>
      <div class="label">帧大小 (KB)</div>
      <div class="value" id="s-size">--</div>
    </div>
    <div>
      <div class="label">已传输</div>
      <div class="value" id="s-mb">--</div>
    </div>
    <div>
      <div class="label"><span id="conn-dot"></span>连接</div>
      <div class="value" id="s-conn">--</div>
    </div>
  </div>
</div>

<div id="perf-bar"><div id="perf-fill" style="width:0%"></div></div>

<div id="tap-hint">点击屏幕切换 HUD</div>

<script>
  var statsUrl = '/api/stats';
  var hudVisible = true;
  var lastFps = 0;

  // Toggle HUD on tap
  document.addEventListener('click', function() {
    hudVisible = !hudVisible;
    var hud = document.getElementById('hud');
    var hint = document.getElementById('tap-hint');
    if (hudVisible) {
      hud.classList.remove('hidden');
      hint.style.opacity = '1';
      setTimeout(function() { hint.style.opacity = '0'; }, 2000);
    } else {
      hud.classList.add('hidden');
      hint.style.opacity = '0';
    }
  });

  // Auto-hide hint after 5s
  setTimeout(function() {
    document.getElementById('tap-hint').style.opacity = '0';
  }, 5000);

  // Poll stats every 500ms
  function fetchStats() {
    fetch(statsUrl)
      .then(function(r) { return r.json(); })
      .then(function(s) {
        updateStats(s);
      })
      .catch(function() {
        document.getElementById('conn-dot').classList.add('dead');
      });
  }

  function updateStats(s) {
    document.getElementById('conn-dot').classList.remove('dead');

    lastFps = s.fps || 0;
    var latMs = s.frame_interval_avg_us ? (s.frame_interval_avg_us / 1000).toFixed(1) : '--';

    document.getElementById('s-fps').textContent = s.fps ? s.fps.toFixed(1) : '--';
    document.getElementById('s-lat').textContent = latMs;
    document.getElementById('s-bw').textContent = s.bandwidth_kbps ? Math.round(s.bandwidth_kbps) : '--';
    document.getElementById('s-size').textContent = s.frame_avg_kb ? s.frame_avg_kb.toFixed(1) : '--';
    document.getElementById('s-mb').textContent = s.total_mb ? s.total_mb.toFixed(1) + ' MB' : '--';
    document.getElementById('s-conn').textContent = (s.active_clients||0) + ' 客户端';

    // Color-code FPS
    var fpsEl = document.getElementById('s-fps');
    fpsEl.className = 'value';
    if (lastFps < 10) fpsEl.classList.add('bad');
    else if (lastFps < 20) fpsEl.classList.add('warn');

    // Performance bar (relative to target 30fps)
    var pct = Math.min(100, (lastFps / 30) * 100);
    var fill = document.getElementById('perf-fill');
    fill.style.width = pct + '%';
    if (lastFps < 10) fill.style.background = '#f00';
    else if (lastFps < 20) fill.style.background = '#f80';
    else fill.style.background = '#0f0';
  }

  function onStreamError() {
    document.getElementById('conn-dot').classList.add('dead');
    document.getElementById('s-fps').textContent = 'ERR';
    document.getElementById('s-fps').className = 'value bad';
  }

  function onStreamLoad() {
    document.getElementById('conn-dot').classList.remove('dead');
  }

  setInterval(fetchStats, 500);
  fetchStats();
</script>

</body>
</html>
)HTML";
    }
};

// ====================================================================
//  Signal handling for graceful shutdown
// ====================================================================

static std::atomic<bool> g_running(true);

static void signal_handler(int) {
    g_running = false;
}

// ====================================================================
//  Main
// ====================================================================

static void print_usage(const char* prog) {
    std::printf(
        "Usage: %s [device] [port] [width] [height] [fps]\n"
        "Defaults: /dev/video0 8080 640 480 30\n"
        "\n"
        "LAN Video Streaming Benchmark with built-in HTTP server & FPV web UI.\n"
        "Open http://<ip>:<port> in a browser on the same LAN to view the FPV stream.\n"
        "\n"
        "Examples:\n"
        "  %s                                       # default\n"
        "  %s /dev/video0 8080 1280 720 30          # 720p @ 30fps\n"
        "  %s /dev/video2 9090 640 480 15           # low-fps test\n"
        "\n"
        "Endpoints:\n"
        "  /            FPV viewer page with real-time stats\n"
        "  /stream      Raw MJPEG stream (for VLC / custom clients)\n"
        "  /api/stats   JSON performance statistics\n"
        "\n"
        "Hotkeys (terminal):\n"
        "  q / Ctrl+C   Graceful shutdown\n",
        prog, prog, prog, prog
    );
}

int main(int argc, char* argv[]) {
    // Parse arguments
    const char* device = "/dev/video0";
    int port = 8080;
    int width = 640;
    int height = 480;
    int fps = 30;

    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (argv[i][0] != '-') {
            switch (positional++) {
                case 0: device = argv[i]; break;
                case 1: port   = std::atoi(argv[i]); break;
                case 2: width  = std::atoi(argv[i]); break;
                case 3: height = std::atoi(argv[i]); break;
                case 4: fps    = std::atoi(argv[i]); break;
            }
        }
    }

    std::printf("═══════════════════════════════════════════════════════\n");
    std::printf("  LAN Video Streaming Benchmark\n");
    std::printf("═══════════════════════════════════════════════════════\n");
    std::printf("  Camera:  %s\n", device);
    std::printf("  Size:    %dx%d\n", width, height);
    std::printf("  Target:  %d fps\n", fps);
    std::printf("  Port:    %d\n", port);
    std::printf("\n");

    // Open camera
    V4L2Camera cam(device, width, height, fps);
    if (!cam.good()) {
        std::fprintf(stderr, "FATAL: camera init failed\n");
        return 1;
    }
    if (!cam.is_mjpeg()) {
        std::fprintf(stderr, "WARNING: camera is not MJPEG — stream may not be viewable in browser\n");
    }

    std::printf("Camera opened: %dx%d @%dfps %s\n\n",
        cam.width(), cam.height(), fps, cam.is_mjpeg() ? "MJPEG" : "YUYV");

    // Shared state
    StreamStats stats;
    FrameBuffer frame_buf;

    // Start HTTP server
    HttpServer server(port, stats, frame_buf);
    if (!server.start()) {
        std::fprintf(stderr, "FATAL: HTTP server failed to start\n");
        return 1;
    }

    // Signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Timing initialization
    stats.start_time_us = now_us();
    stats.last_frame_ts_us = stats.start_time_us;

    std::printf("═══════════════════════════════════════════════════════\n");
    std::printf("  Server running!\n");
    std::printf("  Open in browser:  http://%s:%d\n", stats.server_ip.c_str(), port);
    std::printf("  Stats API:        http://%s:%d/api/stats\n", stats.server_ip.c_str(), port);
    std::printf("  Raw stream:       http://%s:%d/stream\n", stats.server_ip.c_str(), port);
    std::printf("═══════════════════════════════════════════════════════\n");
    std::printf("\nPress 'q' + Enter or Ctrl+C to stop\n\n");

    // ── Main capture loop ──────────────────────────────────────────
    int cam_fd = cam.fd();
    const int poll_timeout_ms = 1000;
    double last_report_ts = now_us();
    int frames_this_second = 0;

    while (g_running) {
        // Poll camera for new frame
        struct pollfd pfd;
        pfd.fd = cam_fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, poll_timeout_ms);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) {
            // Timeout — check for stdin 'q' command
            struct pollfd stdin_pfd = {STDIN_FILENO, POLLIN, 0};
            if (poll(&stdin_pfd, 1, 0) > 0) {
                char c;
                if (read(STDIN_FILENO, &c, 1) == 1 && (c == 'q' || c == 'Q'))
                    break;
            }
            continue;
        }

        // Measure DQBUF latency
        double dq_t0 = now_us();
        auto frame = cam.read_frame();
        double dq_t1 = now_us();

        if (!frame || frame->empty()) {
            stats.dropped_frames++;
            continue;
        }

        size_t len = frame->size();
        double dq_elapsed = dq_t1 - dq_t0;
        double frame_interval = dq_t1 - stats.last_frame_ts_us;

        // Update stats
        {
            std::lock_guard<std::mutex> lock(stats.mtx);
            stats.capture_latency_us.push(dq_elapsed);
            stats.frame_interval_us.push(frame_interval);
            stats.frame_bytes.push(len);
            stats.total_frames++;
            stats.total_bytes += len;
            stats.last_frame_ts_us = dq_t1;
        }

        // Publish frame for HTTP streaming (shared_ptr — no data copy)
        frame_buf.publish(frame);

        // Periodic console report
        frames_this_second++;
        double now = now_us();
        if (now - last_report_ts >= 2'000'000.0) {  // every 2 seconds
            double elapsed = (now - last_report_ts) / 1'000'000.0;
            double actual_fps = frames_this_second / elapsed;
            double bw = (len * frames_this_second * 8.0 / 1000.0) / elapsed;

            std::printf("  [%s] FPS: %5.1f | 带宽: %7.1f kbps | 帧大小: %5.1f KB | "
                        "捕获: %5.0f µs | 发送: %5.0f µs | 客户端: %d\r",
                format_timestamp().c_str(),
                actual_fps, bw, len / 1024.0,
                stats.capture_latency_us.avg(),
                stats.send_latency_us.avg(),
                (int)stats.active_clients);
            std::fflush(stdout);

            frames_this_second = 0;
            last_report_ts = now;
        }
    }

    // ── Shutdown ────────────────────────────────────────────────────
    std::printf("\n\nShutting down...\n");

    server.stop();

    // Final report
    double elapsed = stats.elapsed_sec();
    double avg_fps = elapsed > 0 ? stats.total_frames / elapsed : 0;
    double total_mb = stats.total_bytes / (1024.0 * 1024.0);
    double avg_bw = elapsed > 0 ? (stats.total_bytes * 8.0 / 1000.0) / elapsed : 0;

    std::printf("\n");
    std::printf("═══════════════════════════════════════════════════════\n");
    std::printf("  Streaming Benchmark Results\n");
    std::printf("═══════════════════════════════════════════════════════\n");
    std::printf("  Duration:          %8.1f sec\n", elapsed);
    std::printf("  Total frames:      %8lu\n", (unsigned long)stats.total_frames);
    std::printf("  Average FPS:       %8.1f\n", avg_fps);
    std::printf("  Dropped frames:    %8lu\n", (unsigned long)stats.dropped_frames);
    std::printf("  Total data:        %8.1f MB\n", total_mb);
    std::printf("  Avg bandwidth:     %8.1f kbps\n", avg_bw);
    std::printf("\n");
    std::printf("  ── Capture latency (DQBUF) ──\n");
    std::printf("    Avg:  %8.0f µs\n", stats.capture_latency_us.avg());
    std::printf("    Max:  %8.0f µs\n", stats.capture_latency_us.max());
    std::printf("    Min:  %8.0f µs\n", stats.capture_latency_us.min());
    std::printf("\n");
    std::printf("  ── Send latency (write to socket) ──\n");
    std::printf("    Avg:  %8.0f µs\n", stats.send_latency_us.avg());
    std::printf("    Max:  %8.0f µs\n", stats.send_latency_us.max());
    std::printf("\n");
    std::printf("  ── Frame interval ──\n");
    std::printf("    Avg:  %8.0f µs\n", stats.frame_interval_us.avg());
    std::printf("    Max:  %8.0f µs\n", stats.frame_interval_us.max());
    std::printf("═══════════════════════════════════════════════════════\n");

    return 0;
}
