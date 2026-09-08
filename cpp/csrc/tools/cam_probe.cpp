// cam_probe.cpp — V4L2 摄像头"真档"扫描器（SG2002 / riscv64，静态）
//
// 背景：廉价 UVC 摄像头常出现"列表里有某尺寸、S_FMT 也接受、但固件根本不出帧"
// （假档）。本工具把设备枚举出的每个 格式×分辨率 都真实开流测一遍，
// 报告哪些档真正出帧、帧率、每帧字节数 —— 一次拿到全量真相。
//
// 用法:
//   cam_probe [/dev/video0] [-t 1500] [-m MJPG,YUYV] [-s 320x240] [--list]
//     -t ms     每档测试时长（默认 1500ms）
//     -m FMT    只测指定格式（逗号分隔，默认全部）
//     -s WxH    额外强制测指定尺寸（可多次；即使不在枚举列表里也测）
//     --list    只打印格式表，不做开流测试
//
// 返回码: 0 = 正常完成（有没有真档看输出）

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CLEAR(x) std::memset(&(x), 0, sizeof(x))

static std::string fourcc_name(uint32_t p) {
    char s[5] = {(char)(p & 0xff), (char)((p >> 8) & 0xff),
                 (char)((p >> 16) & 0xff), (char)((p >> 24) & 0xff), 0};
    return std::string(s);
}

static uint32_t fourcc_from_name(const std::string& n) {
    if (n.size() < 4) return 0;
    return (uint32_t)(unsigned char)n[0] | ((uint32_t)(unsigned char)n[1] << 8) |
           ((uint32_t)(unsigned char)n[2] << 16) | ((uint32_t)(unsigned char)n[3] << 24);
}

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Mode {
    uint32_t fourcc;
    int w, h;
};

static void enumerate(int fd, std::vector<Mode>& modes, bool verbose) {
    for (unsigned idx = 0;; idx++) {
        v4l2_fmtdesc fdsc;
        CLEAR(fdsc);
        fdsc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fdsc.index = idx;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fdsc) < 0) break;
        std::string sizes;
        v4l2_frmsizeenum fs;
        CLEAR(fs);
        fs.pixel_format = fdsc.pixelformat;
        while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0) {
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                if (!sizes.empty()) sizes += " ";
                sizes += std::to_string(fs.discrete.width) + "x" + std::to_string(fs.discrete.height);
                modes.push_back({fdsc.pixelformat, (int)fs.discrete.width, (int)fs.discrete.height});
            }
            fs.index++;
        }
        if (verbose)
            std::printf("FMT %s (%s): %s\n", fourcc_name(fdsc.pixelformat).c_str(),
                        (const char*)fdsc.description, sizes.empty() ? "?" : sizes.c_str());
    }
}

// 结果
struct ProbeRes {
    bool tried = false;
    bool opened = false;      // open 成功
    bool fmt_ok = false;      // S_FMT 成功
    bool stream_ok = false;   // STREAMON 成功
    bool frames = false;      // 至少出 1 帧
    int frames_n = 0;
    double fps = 0;
    size_t bytes_total = 0;
    int first_ms = -1;
    int act_w = 0, act_h = 0;
    std::string fmt_err;      // 失败阶段说明
};

static ProbeRes probe_mode(const char* dev, uint32_t fourcc, int req_w, int req_h,
                           int timeout_ms) {
    ProbeRes r;
    r.tried = true;

    int fd = ::open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { r.fmt_err = "open fail"; return r; }
    r.opened = true;

    v4l2_format fmt;
    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = (unsigned)req_w;
    fmt.fmt.pix.height = (unsigned)req_h;
    fmt.fmt.pix.pixelformat = fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        r.fmt_err = "S_FMT fail";
        ::close(fd);
        return r;
    }
    r.fmt_ok = true;
    r.act_w = (int)fmt.fmt.pix.width;
    r.act_h = (int)fmt.fmt.pix.height;
    bool fmt_matched = fmt.fmt.pix.pixelformat == fourcc;

    v4l2_requestbuffers req;
    CLEAR(req);
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        r.fmt_err = "REQBUFS fail";
        ::close(fd);
        return r;
    }
    std::vector<void*> bufs;
    std::vector<size_t> blens;
    bool buf_fail = false;
    for (unsigned i = 0; i < req.count; i++) {
        v4l2_buffer b;
        CLEAR(b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { buf_fail = true; break; }
        void* p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (p == MAP_FAILED) { buf_fail = true; break; }
        bufs.push_back(p);
        blens.push_back(b.length);
    }
    if (buf_fail) {
        for (size_t i = 0; i < bufs.size(); i++) munmap(bufs[i], blens[i]);
        r.fmt_err = "mmap fail";
        ::close(fd);
        return r;
    }
    for (unsigned i = 0; i < bufs.size(); i++) {
        v4l2_buffer b;
        CLEAR(b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        ioctl(fd, VIDIOC_QBUF, &b);
    }
    v4l2_buf_type bt = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &bt) < 0) {
        r.fmt_err = "STREAMON fail";
        for (size_t i = 0; i < bufs.size(); i++) munmap(bufs[i], blens[i]);
        ::close(fd);
        return r;
    }
    r.stream_ok = true;

    // 采集 timeout_ms 看有没有帧
    double deadline = now_ms() + timeout_ms;
    double t0 = now_ms();
    while (now_ms() < deadline) {
        pollfd pfd = {fd, POLLIN, 0};
        int rc = ::poll(&pfd, 1, 200);
        if (rc <= 0) continue;
        v4l2_buffer b;
        CLEAR(b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) continue;
        if (b.index < bufs.size() && bufs[b.index] && b.bytesused > 0) {
            if (r.frames_n == 0) r.first_ms = (int)(now_ms() - t0);
            r.frames_n++;
            r.bytes_total += b.bytesused;
        }
        ioctl(fd, VIDIOC_QBUF, &b);
    }
    double elapsed = now_ms() - t0;
    r.frames = r.frames_n > 0;
    if (elapsed > 0) r.fps = r.frames_n * 1000.0 / elapsed;
    // 协商格式变了也如实标注
    if (!fmt_matched && r.frames) {
        // uvcvideo 一般原样返回请求格式；变了说明驱动凑了
        r.fmt_err = "fmt coerced to " + fourcc_name(fmt.fmt.pix.pixelformat);
    }

    ioctl(fd, VIDIOC_STREAMOFF, &bt);
    for (size_t i = 0; i < bufs.size(); i++) munmap(bufs[i], blens[i]);
    ::close(fd);
    return r;
}

static void usage(const char* prog) {
    std::printf("Usage: %s [/dev/video0] [-t ms] [-m FMT,...] [-s WxH] [--list]\n", prog);
}

int main(int argc, char** argv) {
    const char* dev = "/dev/video0";
    int timeout_ms = 1500;
    bool only_list = false;
    std::vector<uint32_t> fmt_filter;      // 空 = 全部
    std::vector<std::pair<int, int>> extra;  // 额外强测尺寸

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--list") { only_list = true; }
        else if ((a == "-t" || a == "--timeout") && i + 1 < argc) {
            timeout_ms = std::atoi(argv[++i]);
            if (timeout_ms < 100) timeout_ms = 100;
        } else if ((a == "-m") && i + 1 < argc) {
            std::string list = argv[++i];
            size_t p = 0;
            while (p <= list.size()) {
                size_t c = list.find(',', p);
                std::string name = list.substr(p, c == std::string::npos ? std::string::npos : c - p);
                uint32_t f = fourcc_from_name(name);
                if (f) fmt_filter.push_back(f);
                if (c == std::string::npos) break;
                p = c + 1;
            }
        } else if ((a == "-s") && i + 1 < argc) {
            std::string s = argv[++i];
            size_t x = s.find('x');
            if (x != std::string::npos) {
                extra.push_back({std::atoi(s.substr(0, x).c_str()),
                                 std::atoi(s.substr(x + 1).c_str())});
            }
        } else if (a[0] != '-') {
            dev = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    int fd = ::open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr, "cannot open %s: %s\n", dev, std::strerror(errno));
        return 1;
    }
    v4l2_capability cap;
    CLEAR(cap);
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        std::fprintf(stderr, "VIDIOC_QUERYCAP: %s\n", std::strerror(errno));
        ::close(fd);
        return 1;
    }
    std::printf("device : %s\n", dev);
    std::printf("driver : %s\n", cap.driver);
    std::printf("card   : %s\n", cap.card);
    std::printf("bus    : %s\n", cap.bus_info);

    std::vector<Mode> modes;
    enumerate(fd, modes, true);
    ::close(fd);

    // 去重（保序）
    std::vector<Mode> uniq;
    for (auto& m : modes) {
        bool dup = false;
        for (auto& u : uniq)
            if (u.fourcc == m.fourcc && u.w == m.w && u.h == m.h) { dup = true; break; }
        if (!dup) uniq.push_back(m);
    }

    if (only_list) {
        std::printf("\n%d mode(s) listed.\n", (int)uniq.size());
        return 0;
    }

    // 组装测试列表
    std::vector<Mode> tests;
    for (auto& m : uniq) {
        if (!fmt_filter.empty() &&
            std::find(fmt_filter.begin(), fmt_filter.end(), m.fourcc) == fmt_filter.end())
            continue;
        tests.push_back(m);
    }
    // 强测尺寸：对每个过滤器格式（或 MJPG+YUYV）都测
    if (!extra.empty()) {
        std::vector<uint32_t> fs = fmt_filter;
        if (fs.empty()) {
            fs.push_back(fourcc_from_name("MJPG"));
            fs.push_back(fourcc_from_name("YUYV"));
        }
        for (auto& [w, h] : extra)
            for (auto f : fs) tests.push_back({f, w, h});
    }

    std::printf("\nprobing %d mode(s), %d ms each ...\n\n", (int)tests.size(), timeout_ms);
    int ok_count = 0;
    for (size_t i = 0; i < tests.size(); i++) {
        Mode& m = tests[i];
        ProbeRes r = probe_mode(dev, m.fourcc, m.w, m.h, timeout_ms);
        std::printf("[%2zu/%zu] %-4s %4dx%-4d -> %s", i + 1, tests.size(),
                    fourcc_name(m.fourcc).c_str(), m.w, m.h,
                    r.fmt_ok ? (std::to_string(r.act_w) + "x" + std::to_string(r.act_h)).c_str()
                             : "-x-");
        if (!r.fmt_ok) {
            std::printf("  FAIL  (%s)\n", r.fmt_err.c_str());
        } else if (!r.stream_ok) {
            std::printf("  FAIL  (%s)\n", r.fmt_err.c_str());
        } else if (!r.frames) {
            std::printf("  NO FRAMES in %dms  <- 假档候选\n", timeout_ms);
        } else {
            ok_count++;
            std::printf("  OK  %.1f fps  avg %.1f KB/frame  first frame %dms%s%s\n",
                        r.fps,
                        r.frames_n ? (double)r.bytes_total / 1024.0 / r.frames_n : 0.0,
                        r.first_ms,
                        r.fmt_err.empty() ? "" : ("  [" + r.fmt_err + "]").c_str(),
                        "");
        }
    }
    std::printf("\nDone. %d/%zu mode(s) actually streamed.\n", ok_count, tests.size());
    return 0;
}
