/*
 * demo_camera.c — ST7796S 屏 摄像头实时预览 demo（纯 C 单文件，无 dora 依赖）
 *
 * 打通"摄像头 → 屏幕"闭环：
 *   V4L2 读 /dev/video0 → 解码 → 旋转 90° + cover 缩放 + RGB565 → 写 /dev/fb0 循环刷新。
 *   两条取流路径：
 *     - 默认 MJPEG：libjpeg 解码（大图自动降采样）
 *     - DEMO_RAW=1：摄像头直接出原始 YUYV（不压缩，类似 BMP），
 *       YUYV → RGB565 一步转换整块拷屏，完全绕开 JPEG 解码
 *
 * 代码全部复用板上已验证的实现思路：
 *   - V4L2 采集（mmap 双缓冲 + poll + DQBUF/QBUF）: camera-node/camera_v4l2.cpp
 *   - JPEG 解码 + 错误桩: demo_image.c / demo2.c
 *   - 旋转缩放查表 + 方向修正 + 写屏: screen-node-cpp/screen-node.cpp
 *
 * 性能：
 *   - 大图（>640 宽）自动用 libjpeg scale_num/scale_denom 降采样解码
 *     （反正要 cover 缩到 320x480，解码量小 4~64 倍，视觉几乎无损）
 *   - 旋转/缩放映射预计算成整数查表，内层无浮点
 *   - 每秒报告 fps + 解码/绘制分步耗时（dec ms/f、draw ms/f），方便定位瓶颈
 *
 * 编译：
 *   # 本机 (macOS) 编译验证（Linux 专属代码自动跳过，仅跑 JPEG 冒烟）：
 *   gcc -O2 -Wall -o demo_camera demo_camera.c \
 *       -I/opt/homebrew/include -L/opt/homebrew/opt/jpeg-turbo/lib -ljpeg
 *
 *   # 板子 (riscv64 musl, SG2002) 交叉编译 —— 与 demo_image.c 共用同一份 libjpeg.a：
 *   riscv64-unknown-linux-musl-gcc -O2 -static -o demo_camera demo_camera.c \
 *       -I../../dora/libs/jpeg ../../dora/libs/jpeg/libjpeg.a
 *
 * 用法（板上）：
 *   ./demo_camera                          # 默认: 原始YUYV + 半屏 160x240 流畅预览
 *   ./demo_camera /dev/video1              # 指定摄像头设备
 *   CAMERA_WIDTH=640 CAMERA_HEIGHT=480 ./demo_camera   # 指定分辨率
 *   SCREEN_ORIENT=1 ./demo_camera          # 方向修正: 0无 1水平翻 2垂直翻 3=180°(默认)
 *   DEMO_SCALE=2 ./demo_camera             # 半屏 160x240（写屏字节 1/4，帧率约 4 倍，实时预览推荐）
 *   DEMO_RAW=1 ./demo_camera               # 摄像头直接出原始 YUYV，免 JPEG 解码一步拷屏（"整帧拷贝"）
 *   DEMO_LISTFMT=1 ./demo_camera           # 枚举摄像头支持的所有格式/分辨率后退出
 *   DEMO_NOISE=0|1|2|3 ./demo_camera       # 脏行检测容差：忽略每通道 0~3 个 LSB（默认 1）
 *   DEMO_EXP_FIX=1 ./demo_camera           # 固定曝光/AWB/增益（治自动曝光抖动导致全帧变化）
 *   DEMO_BENCH=1 ./demo_camera             # 纯写屏带宽基准（整块 vs 逐行，量化每调用开销）
 *   DEMO_TEST=1 ./demo_camera              # 测试图案模式（红绿蓝白 vs 字节交换版，确认屏字节序）
 *   DEMO_SWAP=1 ./demo_camera              # RGB565 高低字节交换
 *   DEMO_BGR=1 ./demo_camera               # RGB565 R/B 通道对调
 *
 * 注意：运行前确保 /sys/class/graphics/fb0/state 为 1（显示引擎开启，程序启动时也会自动开）。
 * Ctrl-C 退出并清理（停流、释放 buffer、清屏）。
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#endif

#include <jpeglib.h>
#include <setjmp.h>

/* ═══════════════════════ 配置 ═══════════════════════ */
#if defined(__linux__)

#define SW 320   /* 屏幕宽 */
#define SH 480   /* 屏幕高 */

/* 方向修正（环境变量 SCREEN_ORIENT）：
 *   0 = 无翻转
 *   1 = 水平翻转（翻转 ox）—— 修"上下颠倒"（源图顶底反了）
 *   2 = 垂直翻转（翻转 oy）—— 修"左右镜像"
 *   3 = 180°（水平+垂直都翻） */
static int g_orient = 3;   /* 默认 180°（本板摄像头画面颠倒），SCREEN_ORIENT=0..3 可覆盖 */

/* 颜色修正（本板 ST7796S 是 SPI 屏，RGB565 字节序/通道序可能与标准不同）：
 *   DEMO_SWAP=1 → 高低字节交换（bswap16）
 *   DEMO_BGR=1  → R/B 通道对调
 * 板上跑 DEMO_TEST=1 看测试图案后，按实际需要组合。 */
static int g_swap16 = 0;
static int g_bgr = 0;

/* DEMO_RAW=1 → 摄像头优先协商原始 YUYV（不压缩，类似 BMP 原始像素），
 * YUYV → RGB565 一步转换整块拷屏，完全绕开 JPEG 解码（"整帧拷贝"思路）。
 * 默认开启（本板实测：320x240 YUYV + 16MHz SPI 下 21fps，JPEG 路径仅 9fps）；
 * 设 DEMO_RAW=0 可回退 MJPEG 优先。 */
static int g_raw_first = 1;

/* 脏行检测容差：DEMO_NOISE=0 精确 / 1 忽略每通道 1 个 LSB（默认）/
 * 2 忽略 2 个 / 3 忽略 3 个。实况视频帧间有传感器噪声（JPEG 量化还会
 * 放大），精确 memcmp 会把静止画面也判成"全行都变"（板上实测 240/240），
 * 掩掉低 bit 后静态行才能真正不重写。 */
static int g_noise_bits = 1;

static uint16_t noise_mask(void) {
    switch (g_noise_bits) {
        case 0:  return 0xFFFF;
        case 2:  return 0xF3CC;   /* R/G/B 各忽略 2 个 LSB */
        case 3:  return 0xF1C0;   /* R/G/B 各忽略 3 个 LSB */
        default: return 0xF7DE;   /* R/G/B 各忽略 1 个 LSB */
    }
}

/* 整行扫描：差异超出容差掩码 → 判"变了"；同时累计全帧最大通道差 maxd。
 * maxd 是诊断关键：小（1~2）= 纯噪声（加大 DEMO_NOISE 即可）；大（≥8）=
 * 真实运动或全局亮度变化（AWB/曝光抖动），那种情况容差再大也没用。 */
static int row_scan(const uint16_t *a, const uint16_t *b, int n,
                    uint16_t mask, int *maxd) {
    int dirty = 0;
    for (int i = 0; i < n; i++) {
        if (((a[i] ^ b[i]) & mask) != 0) dirty = 1;
        int dr = abs(((a[i] >> 11) & 31) - ((b[i] >> 11) & 31));
        int dg = abs(((a[i] >> 5) & 63) - ((b[i] >> 5) & 63));
        int db = abs((a[i] & 31) - (b[i] & 31));
        int d = dr > dg ? dr : dg;
        if (db > d) d = db;
        if (d > *maxd) *maxd = d;
    }
    return dirty;
}

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

#endif  /* __linux__ */

#define DEMO_LOG(fmt, ...) \
    fprintf(stderr, "[demo_camera] " fmt "\n", ##__VA_ARGS__)

#if defined(__linux__)
/* ═══════════════════════ 帧缓冲 /dev/fb0 ═══════════════════════ */
static int       fb_fd = -1;
static uint16_t *fb_base = NULL;
static size_t    fb_words = 0;
static size_t    fb_bytes = 0;

/* 开启显示引擎（demo2.c 要求 state=1 才显示） */
static void enable_display(void) {
    FILE *s = fopen("/sys/class/graphics/fb0/state", "w");
    if (s) { fputs("1", s); fclose(s); }
    FILE *b = fopen("/sys/class/graphics/fb0/blank", "w");
    if (b) { fputs("0", b); fclose(b); }
    DEMO_LOG("display engine enabled (state=1, blank=0)");
}

/* open /dev/fb0 → FBIOGET_VSCREENINFO → mmap RGB565 写屏 */
static int open_fb(void) {
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        DEMO_LOG("open /dev/fb0 failed: %s", strerror(errno));
        return 0;
    }
    /* FBIOGET_VSCREENINFO = 0x4600 (linux/fb.h 标准值) */
    struct { unsigned xres, yres, xres_v, yres_v, xoff, yoff, bpp; } vi;
    memset(&vi, 0, sizeof vi);
    if (ioctl(fb_fd, 0x4600, &vi) == 0 && vi.xres > 0 && vi.bpp > 0) {
        fb_words = (size_t)vi.xres * vi.yres;
        fb_bytes = fb_words * (vi.bpp / 8);
    } else {
        DEMO_LOG("FBIOGET_VSCREENINFO failed, assuming %dx%d @16bpp", SW, SH);
        fb_words = SW * SH;
        fb_bytes = fb_words * 2;
    }
    void *p = mmap(NULL, fb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (p == MAP_FAILED) {
        DEMO_LOG("mmap /dev/fb0 failed: %s", strerror(errno));
        close(fb_fd); fb_fd = -1;
        return 0;
    }
    fb_base = (uint16_t *)p;
    DEMO_LOG("fb0 mapped %zu bytes (%zu px)", fb_bytes, fb_words);
    return 1;
}

static void close_fb(void) {
    if (fb_base) { munmap(fb_base, fb_bytes); fb_base = NULL; }
    if (fb_fd >= 0) { close(fb_fd); fb_fd = -1; }
}

static void fb_fill(uint16_t color) {
    if (!fb_base) return;
    for (size_t i = 0; i < fb_words; i++) fb_base[i] = color;
}

/* ═══════════════════════ V4L2 摄像头 ═══════════════════════ */
/* 简化自 camera-node/camera_v4l2.cpp：MMAP 双缓冲 + poll 取帧。
 * 注意：v4l2_capability 的 driver/card 不保证 NUL 结尾，打印前必须
 * 复制到定长缓冲，直接 %s 会越界读（板上曾因此段错误）。 */

struct CamBuf { void *ptr; size_t len; };

static int       cam_fd = -1;
static uint32_t  cam_fmt = 0;          /* V4L2_PIX_FMT_MJPEG 或 V4L2_PIX_FMT_YUYV */
static int       cam_w = 0, cam_h = 0;
static unsigned  cam_nbufs = 0;
static struct CamBuf *cam_bufs = NULL;
static uint8_t  *cam_frame = NULL;
static size_t    cam_frame_cap = 0;

static int try_fmt(int *w, int *h) {
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width       = (unsigned)*w;
    f.fmt.pix.height      = (unsigned)*h;
    f.fmt.pix.pixelformat = cam_fmt;
    f.fmt.pix.field       = V4L2_FIELD_ANY;
    if (ioctl(cam_fd, VIDIOC_S_FMT, &f) < 0) return 0;
    *w = (int)f.fmt.pix.width;
    *h = (int)f.fmt.pix.height;
    return f.fmt.pix.pixelformat == cam_fmt;
}

static int init_buffers(void) {
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof req);
    req.count  = 2;   /* 减少缓冲数降低延迟（与 camera-node 一致） */
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cam_fd, VIDIOC_REQBUFS, &req) < 0) {
        DEMO_LOG("V4L2: VIDIOC_REQBUFS: %s", strerror(errno));
        return 0;
    }
    cam_nbufs = req.count;
    cam_bufs = calloc(cam_nbufs, sizeof *cam_bufs);
    if (!cam_bufs) return 0;
    for (unsigned i = 0; i < cam_nbufs; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = i;
        if (ioctl(cam_fd, VIDIOC_QUERYBUF, &b) < 0) {
            DEMO_LOG("V4L2: QUERYBUF[%u]: %s", i, strerror(errno));
            return 0;
        }
        cam_bufs[i].len = b.length;
        cam_bufs[i].ptr = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, cam_fd, b.m.offset);
        if (cam_bufs[i].ptr == MAP_FAILED) {
            cam_bufs[i].ptr = NULL;   /* 防 close_camera 误 munmap */
            DEMO_LOG("V4L2: mmap[%u]: %s", i, strerror(errno));
            return 0;
        }
    }
    return 1;
}

static int start_stream(void) {
    for (unsigned i = 0; i < cam_nbufs; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = i;
        ioctl(cam_fd, VIDIOC_QBUF, &b);
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return ioctl(cam_fd, VIDIOC_STREAMON, &t) == 0;
}

static void close_camera(void);   /* 前向声明（open_camera 的错误路径会调用） */

static int g_list_fmt = 0;        /* DEMO_LISTFMT=1 → 枚举摄像头格式/分辨率后退出 */
static int g_exp_fix = 0;         /* DEMO_EXP_FIX=1 → 关自动曝光/AWB/增益，固定当前值 */

/* 固定曝光/白平衡/增益（DEMO_EXP_FIX=1，best-effort）。
 * 廉价 UVC 摄像头的自动曝光/自动白平衡会周期性抖动 → 全局亮度每帧变化、
 * 所有行都"变"（板上实测 maxΔ 周期性飙到 50+），脏行检测完全失效。
 * 关掉自动控制并写回当前值后，静止画面才能真正静止。不支持的控件忽略。 */
static void fix_exposure(void) {
    struct v4l2_control c;

    /* 1. 关自动白平衡 */
    memset(&c, 0, sizeof c);
    c.id = V4L2_CID_AUTO_WHITE_BALANCE; c.value = 0;
    DEMO_LOG("EXP_FIX: AWB off %s", ioctl(cam_fd, VIDIOC_S_CTRL, &c) == 0 ? "ok" : "unsupported");

    /* 2. 关自动增益 → 手动，写回当前增益值 */
    memset(&c, 0, sizeof c);
    c.id = V4L2_CID_GAIN;
    if (ioctl(cam_fd, VIDIOC_G_CTRL, &c) == 0) {
        int gain = c.value;
        memset(&c, 0, sizeof c);
        c.id = V4L2_CID_AUTOGAIN; c.value = 0;       /* 手动增益 */
        ioctl(cam_fd, VIDIOC_S_CTRL, &c);
        memset(&c, 0, sizeof c);
        c.id = V4L2_CID_GAIN; c.value = gain;        /* 写回固定增益 */
        ioctl(cam_fd, VIDIOC_S_CTRL, &c);
        DEMO_LOG("EXP_FIX: gain fixed at %d", gain);
    }

    /* 3. 关自动曝光 → 手动，写回当前曝光值 */
    memset(&c, 0, sizeof c);
    c.id = V4L2_CID_EXPOSURE_ABSOLUTE;
    if (ioctl(cam_fd, VIDIOC_G_CTRL, &c) == 0) {
        int exp = c.value;
        memset(&c, 0, sizeof c);
        c.id = V4L2_CID_EXPOSURE_AUTO; c.value = 1;  /* V4L2_EXPOSURE_MANUAL */
        ioctl(cam_fd, VIDIOC_S_CTRL, &c);
        memset(&c, 0, sizeof c);
        c.id = V4L2_CID_EXPOSURE_ABSOLUTE; c.value = exp;
        ioctl(cam_fd, VIDIOC_S_CTRL, &c);
        DEMO_LOG("EXP_FIX: exposure fixed at %d", exp);
    }

    /* 4. 背光补偿关掉（部分驱动默认开启会周期性调亮度） */
    memset(&c, 0, sizeof c);
    c.id = V4L2_CID_BACKLIGHT_COMPENSATION; c.value = 0;
    ioctl(cam_fd, VIDIOC_S_CTRL, &c);
}

/* 枚举摄像头支持的所有格式 + 分辨率（验证摄像头能否出某种格式/分辨率） */
static void enumerate_formats(void) {
    struct v4l2_fmtdesc fd;
    memset(&fd, 0, sizeof fd);
    fd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (ioctl(cam_fd, VIDIOC_ENUM_FMT, &fd) == 0) {
        char fourcc[5] = {(char)fd.pixelformat, (char)(fd.pixelformat >> 8),
                          (char)(fd.pixelformat >> 16), (char)(fd.pixelformat >> 24), 0};
        DEMO_LOG("format %s 0x%08X \"%s\"", fourcc, fd.pixelformat, fd.description);
        struct v4l2_frmsizeenum fs;
        memset(&fs, 0, sizeof fs);
        fs.pixel_format = fd.pixelformat;
        fs.index = 0;
        while (ioctl(cam_fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0) {
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                DEMO_LOG("    %ux%u", fs.discrete.width, fs.discrete.height);
            else if (fs.type == V4L2_FRMSIZE_TYPE_STEPWISE)
                DEMO_LOG("    stepwise %u..%u x %u..%u",
                         fs.stepwise.min_width, fs.stepwise.max_width,
                         fs.stepwise.min_height, fs.stepwise.max_height);
            fs.index++;
        }
        fd.index++;
    }
}

/* 打开摄像头：默认优先 MJPEG；DEMO_RAW=1 时优先原始 YUYV（免解码直转写屏）。
 * 返回 1 成功；返回 2 表示 DEMO_LISTFMT 枚举完已退出。 */
static int open_camera(const char *device, int target_w, int target_h) {
    if (!device) device = "/dev/video0";
    DEMO_LOG("opening camera %s (%dx%d)%s", device, target_w, target_h,
             g_raw_first ? " [raw YUYV preferred]" : "");
    cam_fd = open(device, O_RDWR | O_NONBLOCK);
    if (cam_fd < 0) {
        DEMO_LOG("V4L2: cannot open %s: %s", device, strerror(errno));
        return 0;
    }
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (ioctl(cam_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        DEMO_LOG("V4L2: QUERYCAP: %s", strerror(errno));
        close_camera();
        return 0;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        DEMO_LOG("V4L2: %s not a streaming capture device (caps=0x%08x)",
                 device, cap.capabilities);
        close_camera();
        return 0;
    }
    if (g_list_fmt) {
        enumerate_formats();
        close_camera();
        return 2;
    }
    /* driver/card 不保证 NUL 结尾 → 定长缓冲再 %s，防止越界读段错误 */
    char drv[17] = {0}, crd[33] = {0};
    memcpy(drv, cap.driver, 16);
    memcpy(crd, cap.card, 32);
    DEMO_LOG("V4L2 driver=%s card=%s caps=0x%08x", drv, crd, cap.capabilities);

    if (g_raw_first) {
        /* DEMO_RAW：先试 YUYV（原始帧，免 JPEG），失败再回退 MJPEG */
        cam_w = target_w; cam_h = target_h; cam_fmt = V4L2_PIX_FMT_YUYV;
        if (!try_fmt(&cam_w, &cam_h)) {
            cam_w = target_w; cam_h = target_h; cam_fmt = V4L2_PIX_FMT_MJPEG;
            if (!try_fmt(&cam_w, &cam_h)) {
                DEMO_LOG("V4L2: neither YUYV nor MJPEG supported");
                close_camera();
                return 0;
            }
        }
    } else {
        cam_w = target_w; cam_h = target_h; cam_fmt = V4L2_PIX_FMT_MJPEG;
        if (!try_fmt(&cam_w, &cam_h)) {
            cam_w = target_w; cam_h = target_h; cam_fmt = V4L2_PIX_FMT_YUYV;
            if (!try_fmt(&cam_w, &cam_h)) {
                DEMO_LOG("V4L2: neither MJPEG nor YUYV supported");
                close_camera();
                return 0;
            }
        }
    }
    DEMO_LOG("V4L2 negotiated %s %dx%d",
             cam_fmt == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV", cam_w, cam_h);

    /* DEMO_EXP_FIX=1：开流前固定曝光/AWB/增益（自动控制会周期性抖动，
     * 让静止画面也全帧变化，脏行检测失效） */
    if (g_exp_fix) fix_exposure();

    /* 请求 30fps（best-effort，与 camera-node 的 _configure_mjpeg 一致）；
     * 部分驱动不设 timeperframe 会按很低的默认帧率出图。 */
    {
        struct v4l2_streamparm parm;
        memset(&parm, 0, sizeof parm);
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(cam_fd, VIDIOC_G_PARM, &parm) == 0 &&
            (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
            parm.parm.capture.timeperframe.numerator   = 1;
            parm.parm.capture.timeperframe.denominator = 30;
            ioctl(cam_fd, VIDIOC_S_PARM, &parm);
        }
    }

    if (!init_buffers() || !start_stream()) {
        close_camera();
        return 0;
    }
    DEMO_LOG("V4L2 %dx%d %s (target %dx%d)",
             cam_w, cam_h, cam_fmt == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV",
             target_w, target_h);
    return 1;
}

static void close_camera(void) {
    if (cam_fd >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(cam_fd, VIDIOC_STREAMOFF, &t);
        close(cam_fd);
        cam_fd = -1;
    }
    if (cam_bufs) {
        for (unsigned i = 0; i < cam_nbufs; i++) {
            if (cam_bufs[i].ptr && cam_bufs[i].ptr != MAP_FAILED)
                munmap(cam_bufs[i].ptr, cam_bufs[i].len);
        }
        free(cam_bufs);
        cam_bufs = NULL;
        cam_nbufs = 0;
    }
    free(cam_frame);
    cam_frame = NULL;
    cam_frame_cap = 0;
}

/* 等待一帧（最多 timeout_ms），返回 1 表示 fd 可读 */
static int wait_frame(int timeout_ms) {
    struct pollfd pfd = {cam_fd, POLLIN, 0};
    return poll(&pfd, 1, timeout_ms) > 0;
}

/* 取一帧：DQBUF → 拷到内部缓冲 → QBUF。返回 (data, len)，失败为 NULL。 */
static const uint8_t *read_frame(size_t *out_len) {
    struct v4l2_buffer b;
    memset(&b, 0, sizeof b);
    b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cam_fd, VIDIOC_DQBUF, &b) < 0) return NULL;
    if (!cam_bufs || b.index >= cam_nbufs || !cam_bufs[b.index].ptr) {
        ioctl(cam_fd, VIDIOC_QBUF, &b);   /* 归还异常 buffer，防死锁 */
        return NULL;
    }
    size_t len = b.bytesused;
    if (len > cam_frame_cap) {
        uint8_t *np = realloc(cam_frame, len);
        if (!np) { ioctl(cam_fd, VIDIOC_QBUF, &b); return NULL; }
        cam_frame = np;
        cam_frame_cap = len;
    }
    memcpy(cam_frame, cam_bufs[b.index].ptr, len);
    ioctl(cam_fd, VIDIOC_QBUF, &b);
    *out_len = len;
    return cam_frame;
}

#endif  /* __linux__ */

/* ═══════════════════════ JPEG / YUYV → RGB8 ═══════════════════════ */

/* libjpeg 错误桩：setjmp/longjmp 跳出解码（坏帧不拖死 demo）。
 * jpeg_error_mgr 必须是结构体首成员，cinfo->err 才能转型回 JpegErr* 拿 jmp_buf。 */
struct JpegErr {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};
static void jpeg_on_error(j_common_ptr cinfo) {
    char buf[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buf);
    DEMO_LOG("JPEG decode error: %s", buf);
    longjmp(((struct JpegErr *)cinfo->err)->jump, 1);
}

#if !defined(__linux__)
/* 只读 JPEG header 拿尺寸（不整帧解码）—— 仅 host 冒烟测试用 */
static int jpeg_get_size(const unsigned char *src, size_t src_len, int *w, int *h) {
    struct jpeg_decompress_struct cinfo;
    struct JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, src, src_len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }
    *w = (int)cinfo.image_width;
    *h = (int)cinfo.image_height;
    jpeg_destroy_decompress(&cinfo);
    return 1;
}
#endif  /* !__linux__ */

#if defined(__linux__)

/* 解码 JPEG → RGB888，按需扩容 *buf。
 *
 * 性能关键：本板摄像头若输出高分辨率帧（如 640x480 / 1280x720），通用
 * libjpeg 整帧解码在 C906 上要几百毫秒 → 帧率暴跌。反正最后要 cover 缩到
 * 320x480，这里用 libjpeg 的 scale_num/scale_denom 先做 1/2..1/8 降采样
 * 解码（输出宽 > 640 就降一档），解码量小 4~64 倍，视觉几乎无损。
 * 320x240 的源不降采样，保持原画质。 */
static int jpeg_to_rgb(const unsigned char *src, size_t src_len,
                       unsigned char **buf, size_t *cap,
                       int *out_w, int *out_h) {
    struct jpeg_decompress_struct cinfo;
    struct JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, src, src_len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return 0;
    }
    cinfo.out_color_space = JCS_RGB;   /* 强制 RGB，避免 CMYK/YCCK */
    unsigned scale = 1;
    while ((cinfo.image_width / scale) > 640 && scale < 8) scale <<= 1;
    cinfo.scale_num   = 1;
    cinfo.scale_denom = scale;
    jpeg_start_decompress(&cinfo);
    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    size_t need = (size_t)w * h * 3;
    if (need > *cap) {
        unsigned char *np = realloc(*buf, need);
        if (!np) { jpeg_destroy_decompress(&cinfo); return 0; }
        *buf = np;
        *cap = need;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *row = *buf + (size_t)cinfo.output_scanline * w * 3;
        jpeg_read_scanlines(&cinfo, (JSAMPARRAY)&row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    *out_w = w;
    *out_h = h;
    return 1;
}

/* RGB8 → RGB565 写屏：旋转 90°（顺时针）+ cover 拉伸到 SWxSH 竖屏。
 *
 * RGB8 布局：每像素 3 字节 (r g b)，行宽 = w*3。src 为 w x h 的 RGB 帧。
 * 旋转后图像尺寸：宽 = h（源高），高 = w（源宽）。输出竖屏 (ox,oy) 逐像素
 * 反推到源：oy 对应旋转后"垂直"方向（范围 0..w），ox 对应"水平"方向（0..h）。
 *
 * 性能：riscv64 软浮点做逐像素 double 除法极慢（320x480=15 万像素），
 * 这里把列→源行 / 行→源列映射预计算成整数查表，内层只剩整数运算。 */
static void rgb_to_fb(const uint8_t *rgb, int w, int h,
                      uint16_t *out, int out_w, int out_h) {
    static int sy_map[SW];   /* 输出列 ox → 源行 sy */
    static int sx_map[SH];   /* 输出行 oy → 源列 sx */
    const double sw = (double)h;      /* 旋转后宽 */
    const double sh = (double)w;      /* 旋转后高 */
    const double s  = (out_w / sw) > (out_h / sh) ? (out_w / sw) : (out_h / sh);
    const double off_x = (sw * s - out_w) / 2.0;   /* 水平居中裁切 */
    const double off_y = (sh * s - out_h) / 2.0;   /* 垂直居中裁切 */
    const size_t stride = (size_t)w * 3;

    for (int ox = 0; ox < out_w; ++ox) {
        int sy = (int)(h - 1 - (off_x + ox) / s + 0.5);
        if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;
        sy_map[ox] = sy;
    }
    for (int oy = 0; oy < out_h; ++oy) {
        int sx = (int)((off_y + oy) / s + 0.5);
        if (sx < 0) sx = 0; else if (sx >= w) sx = w - 1;
        sx_map[oy] = sx;
    }

    for (int oy = 0; oy < out_h; ++oy) {
        const size_t col_off = (size_t)sx_map[oy] * 3;   /* 源列字节偏移 */
        /* 方向修正：位0 (值1) = 水平翻转（翻转 ox），位1 (值2) = 垂直翻转（翻转 oy） */
        const int dst_oy = (g_orient & 2) ? (out_h - 1 - oy) : oy;
        uint16_t *dst = out + (size_t)dst_oy * out_w;
        for (int ox = 0; ox < out_w; ++ox) {
            const int dst_ox = (g_orient & 1) ? (out_w - 1 - ox) : ox;
            const uint8_t *p = rgb + (size_t)sy_map[ox] * stride + col_off;
            uint16_t v = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
            if (g_bgr)    /* R/B 通道对调（部分 SPI 屏要 BGR565） */
                v = (uint16_t)(((v & 0x001Fu) << 11) | (v & 0x07E0u) | ((v & 0xF800u) >> 11));
            if (g_swap16) /* 高低字节交换（部分 SPI 屏要 bswap16） */
                v = (uint16_t)((v >> 8) | (v << 8));
            dst[dst_ox] = v;
        }
    }
}

/* YUYV → RGB565 一步写屏（DEMO_RAW 路径，"整帧拷贝"思路）：
 * 源是摄像头原始 YUYV 帧（每像素 2 字节，类似 BMP 原始像素），不经 JPEG
 * 解码、不经 RGB8 中间缓冲，LUT 旋转/缩放 + YCbCr→RGB565 一次完成，
 * 结果直接给 fb_blit_region 整块拷屏。布局与 rgb_to_fb 相同：
 * 输出 (ox,oy) → 源 (sy,sx)；U/V 从宏像素 (sx & ~1) 取，否则奇数像素反色。 */
static void yuyv_to_fb(const uint8_t *yuyv, int w, int h,
                       uint16_t *out, int out_w, int out_h) {
    static int sy_map[SW];   /* 输出列 ox → 源行 sy */
    static int sx_map[SH];   /* 输出行 oy → 源列 sx */
    const double sw = (double)h;      /* 旋转后宽 */
    const double sh = (double)w;      /* 旋转后高 */
    const double s  = (out_w / sw) > (out_h / sh) ? (out_w / sw) : (out_h / sh);
    const double off_x = (sw * s - out_w) / 2.0;   /* 水平居中裁切 */
    const double off_y = (sh * s - out_h) / 2.0;   /* 垂直居中裁切 */

    for (int ox = 0; ox < out_w; ++ox) {
        int sy = (int)(h - 1 - (off_x + ox) / s + 0.5);
        if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;
        sy_map[ox] = sy;
    }
    for (int oy = 0; oy < out_h; ++oy) {
        int sx = (int)((off_y + oy) / s + 0.5);
        if (sx < 0) sx = 0; else if (sx >= w) sx = w - 1;
        sx_map[oy] = sx;
    }

    for (int oy = 0; oy < out_h; ++oy) {
        const size_t col_off   = (size_t)sx_map[oy] * 2;   /* 源列字节偏移 */
        const size_t macro_off = col_off & ~3u;            /* 宏像素 U/V 起点 */
        const int dst_oy = (g_orient & 2) ? (out_h - 1 - oy) : oy;
        uint16_t *dst = out + (size_t)dst_oy * out_w;
        for (int ox = 0; ox < out_w; ++ox) {
            const int dst_ox = (g_orient & 1) ? (out_w - 1 - ox) : ox;
            const size_t row_off = (size_t)sy_map[ox] * w * 2;
            const uint8_t *p = yuyv + row_off + col_off;
            uint8_t Y = p[0];
            uint8_t U = yuyv[row_off + macro_off + 1];
            uint8_t V = yuyv[row_off + macro_off + 3];
            int C = (int)Y - 16, D = (int)U - 128, E = (int)V - 128;
            int r = (298 * C + 409 * E + 128) >> 8;
            int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int b = (298 * C + 516 * D + 128) >> 8;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            if (g_bgr)    /* R/B 通道对调 */
                v = (uint16_t)(((v & 0x001Fu) << 11) | (v & 0x07E0u) | ((v & 0xF800u) >> 11));
            if (g_swap16) /* 高低字节交换 */
                v = (uint16_t)((v >> 8) | (v << 8));
            dst[dst_ox] = v;
        }
    }
}

#endif  /* __linux__ */

/* ═══════════════════════ main ═══════════════════════ */

int main(int argc, char **argv) {
#if defined(__linux__)
    const char *o = getenv("SCREEN_ORIENT");
    if (o) {
        g_orient = atoi(o);
        if (g_orient < 0) g_orient = 0;
        if (g_orient > 3) g_orient = 3;
    }
    DEMO_LOG("Starting (orient=%d)", g_orient);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    /* 分辨率：CAMERA_WIDTH/CAMERA_HEIGHT env（init.sh 从 config.toml 注入），缺省 320x240 */
    int cam_w = 320, cam_h = 240;
    {
        const char *ew = getenv("CAMERA_WIDTH"), *eh = getenv("CAMERA_HEIGHT");
        if (ew) cam_w = atoi(ew);
        if (eh) cam_h = atoi(eh);
        if (cam_w <= 0) cam_w = 320;
        if (cam_h <= 0) cam_h = 240;
    }
    const char *device = (argc >= 2) ? argv[1] : NULL;   /* NULL → /dev/video0 */

    /* 颜色开关 */
    {
        const char *sw = getenv("DEMO_SWAP");
        if (sw && atoi(sw) > 0) g_swap16 = 1;
        const char *bg = getenv("DEMO_BGR");
        if (bg && atoi(bg) > 0) g_bgr = 1;
    }
    /* DEMO_RAW=1 → 摄像头优先出原始 YUYV，免 JPEG 解码一步转 RGB565 拷屏 */
    {
        const char *rw = getenv("DEMO_RAW");
        if (rw && atoi(rw) > 0) g_raw_first = 1;
    }
    /* DEMO_LISTFMT=1 → 枚举摄像头支持格式/分辨率后退出 */
    {
        const char *lf = getenv("DEMO_LISTFMT");
        if (lf && atoi(lf) > 0) g_list_fmt = 1;
    }
    /* DEMO_EXP_FIX=1 → 开流前固定曝光/AWB/增益（治自动曝光抖动导致的全帧变化） */
    {
        const char *ef = getenv("DEMO_EXP_FIX");
        if (ef && atoi(ef) > 0) g_exp_fix = 1;
    }
    /* DEMO_NOISE → 脏行检测容差（0 精确 / 1 默认 / 2 / 3 更宽松） */
    {
        const char *ns = getenv("DEMO_NOISE");
        if (ns) {
            int v = atoi(ns);
            if (v >= 0 && v <= 3) g_noise_bits = v;
        }
    }

    enable_display();
    if (!open_fb()) {
        DEMO_LOG("no /dev/fb0 — running without display");
    } else {
        fb_fill(0x0000);
    }

    /* DEMO_TEST=1：全屏单色顺序播放（每色 ~1.5s，Ctrl-C 退出，不开摄像头）。
     * 4 个 phase 各显示 红→绿→蓝→白，日志带编号 #1~#16：
     *   phase1 原值、phase2 高低字节交换、phase3 R/B对调、phase4 两者都做
     * 观察并回报"实际看到的颜色顺序"（对照日志里的编号），据此确定
     * 本屏需要的 RGB565 变换（纯红显示成白/纯白显示成蓝 → 屏可能不是
     * 标准 16bit RGB565 解析，需要这个测试精确定位）。 */
    {
        const char *tm = getenv("DEMO_TEST");
        if (tm && atoi(tm) > 0 && fb_base) {
            static const uint16_t kColors[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
            static const char *kNames[4] = {"RED", "GREEN", "BLUE", "WHITE"};
            int seq = 1;
            for (int phase = 0; phase < 4 && !g_stop; phase++) {
                const char *pm = phase == 0 ? "normal" :
                                 phase == 1 ? "byte-swap" :
                                 phase == 2 ? "R/B-swap" : "both";
                for (int i = 0; i < 4 && !g_stop; i++) {
                    uint16_t v = kColors[i];
                    if (phase & 1)  /* 高低字节交换 */
                        v = (uint16_t)((v >> 8) | (v << 8));
                    if (phase & 2)  /* R/B 通道对调 */
                        v = (uint16_t)(((v & 0x001Fu) << 11) | (v & 0x07E0u) | ((v & 0xF800u) >> 11));
                    fb_fill(v);
                    DEMO_LOG("TEST #%d [%s] %s -> fill 0x%04X", seq++, pm, kNames[i], v);
                    for (int k = 0; k < 15 && !g_stop; k++) usleep(100000);   /* ~1.5s */
                }
            }
            fb_fill(0x0000);
            close_fb();
            return 0;
        }
    }

    /* DEMO_BENCH=1：纯写屏带宽基准（不开摄像头）——测驱动对不同写法的
     * 吞吐，量化"每次写调用"的固定开销（整块写 vs 逐行写）。
     * 注意：SPI 时钟/模式/DMA 由内核驱动与设备树决定，用户态改不了；
     * 这些数据用于和驱动侧对比、确认瓶颈。 */
    {
        const char *bm = getenv("DEMO_BENCH");
        if (bm && atoi(bm) > 0 && fb_base) {
            static uint16_t buf[SW * SH];
            uint32_t seed = 0x12345678;
            for (int i = 0; i < SW * SH; i++) {
                seed = seed * 1664525u + 1013904223u;
                buf[i] = (uint16_t)(seed >> 16);
            }
            struct timespec t0, t1;
            long best;
            /* A: 全屏整块 memcpy（307200B，1 次调用） */
            best = 0;
            for (int k = 0; k < 3; k++) {
                clock_gettime(CLOCK_MONOTONIC, &t0);
                memcpy(fb_base, buf, (size_t)SW * SH * 2);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
                if (best == 0 || ms < best) best = ms;
            }
            DEMO_LOG("BENCH A: full single memcpy 307200B -> %ldms = %ld KB/s",
                     best, best ? (long)(307200 / best) : 0);
            /* B: 全屏逐行（480 次 × 640B） */
            best = 0;
            for (int k = 0; k < 3; k++) {
                clock_gettime(CLOCK_MONOTONIC, &t0);
                for (int r = 0; r < SH; r++)
                    memcpy(fb_base + (size_t)r * SW, buf + (size_t)r * SW, (size_t)SW * 2);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
                if (best == 0 || ms < best) best = ms;
            }
            DEMO_LOG("BENCH B: full row-wise 480x640B  -> %ldms = %ld KB/s",
                     best, best ? (long)(307200 / best) : 0);
            /* C: 半屏区域逐行（240 次 × 320B，居中） */
            best = 0;
            for (int k = 0; k < 3; k++) {
                uint16_t *dst = fb_base + (size_t)((SH - 240) / 2) * SW + (SW - 160) / 2;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                for (int r = 0; r < 240; r++)
                    memcpy(dst + (size_t)r * SW, buf + (size_t)r * 160, (size_t)160 * 2);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
                if (best == 0 || ms < best) best = ms;
            }
            DEMO_LOG("BENCH C: half region row-wise 240x320B -> %ldms = %ld KB/s",
                     best, best ? (long)(76800 / best) : 0);
            fb_fill(0x0000);
            close_fb();
            return 0;
        }
    }

    int cam_rc = open_camera(device, cam_w, cam_h);
    if (cam_rc == 2) {   /* DEMO_LISTFMT：枚举完退出 */
        close_fb();
        return 0;
    }
    if (!cam_rc) {
        close_fb();
        return 1;
    }

    static uint16_t out[SW * SH];        /* RGB565 帧缓冲（全屏最大尺寸） */
    static uint16_t prev[SW * SH];       /* 上一帧已上屏内容（脏行检测用） */
    unsigned char *rgb = NULL;           /* 解码出的 RGB8 帧 */
    size_t rgb_cap = 0;
    /* 首帧强制全量上屏：prev 全 0xFF，与任何真实画面都不等 */
    memset(prev, 0xFF, sizeof prev);

    /* 显示区域：DEMO_SCALE=n 把屏幕分成 n×n 块显示（n=1..4，居中）。
     * 板上 ST7796S 是 SPI 屏，全屏写 307KB 受屏带宽限制（16MHz 下 ~7fps）；
     * 半屏(scale=2)只写 77KB → 可吃满摄像头 30fps。默认 2（流畅预览），
     * 要看全屏设 DEMO_SCALE=1。 */
    int disp_scale = 2;
    {
        const char *es = getenv("DEMO_SCALE");
        if (es) {
            disp_scale = atoi(es);
            if (disp_scale < 1) disp_scale = 1;
            if (disp_scale > 4) disp_scale = 4;
        }
    }
    const int out_w = SW / disp_scale;
    const int out_h = SH / disp_scale;
    uint16_t *blit_dst = fb_base
        ? fb_base + (size_t)((SH - out_h) / 2) * SW + (SW - out_w) / 2
        : NULL;
    DEMO_LOG("display area %dx%d (%d/%d screen, %zu KB/frame blit)",
             out_w, out_h, disp_scale, disp_scale,
             (size_t)out_w * out_h * 2 / 1024);

    uint64_t frames = 0;
    int fps_count = 0;
    long acc_dec_ms = 0, acc_scale_ms = 0, acc_blit_ms = 0, acc_rows = 0;
    int acc_maxd = 0;
    struct timespec t_last;
    clock_gettime(CLOCK_MONOTONIC, &t_last);

    while (!g_stop) {
        if (!wait_frame(1000)) continue;   /* 1s 超时兜底，期间响应 Ctrl-C */

        size_t len = 0;
        const unsigned char *data = read_frame(&len);
        if (!data || len == 0) continue;

        struct timespec t0, t1, t1a, t2;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int w = 0, h = 0;
        if (cam_fmt == V4L2_PIX_FMT_YUYV) {
            /* DEMO_RAW 路径：原始 YUYV 一步转 RGB565（旋转/缩放/色度一次完成），
             * 不经过 JPEG 解码和 RGB8 中间缓冲，结果直接给 blit 整块拷屏 */
            w = cam_w; h = cam_h;
            yuyv_to_fb(data, w, h, out, out_w, out_h);
        } else if (cam_fmt == V4L2_PIX_FMT_MJPEG) {
            /* 单趟解码：内部按需扩容 rgb + 大图自动降采样 */
            if (!jpeg_to_rgb(data, len, &rgb, &rgb_cap, &w, &h)) continue;
        } else {
            continue;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);

        if (w > 0 && h > 0) {
            if (cam_fmt == V4L2_PIX_FMT_MJPEG)
                rgb_to_fb(rgb, w, h, out, out_w, out_h);   /* YUYV 已在上面一步完成 */
            clock_gettime(CLOCK_MONOTONIC, &t1a);

            /* 脏行检测：SPI 屏写带宽贵（~870KB/s），只把"内容变了"的行写进
             * fb0 —— 屏本来就按行从左向右扫，静态区域完全不重写。
             * 静态画面/缓慢移动时每帧只写几行 → blit 时间按比例大降；
             * 全画面都在动时每行都变 → 与全量写等价（无额外开销，行扫描仅
             * 约 0.5ms）。逐行粒度是甜点：比整帧省写、又不用像素级管理。 */
            int dirty_rows = 0, max_delta = 0;
            uint16_t mask = noise_mask();
            for (int r = 0; r < out_h; r++) {
                const uint16_t *cur = out + (size_t)r * out_w;
                uint16_t *pv = prev + (size_t)r * out_w;
                if (row_scan(cur, pv, out_w, mask, &max_delta)) {
                    if (blit_dst)
                        memcpy(blit_dst + (size_t)r * SW, cur, (size_t)out_w * 2);
                    memcpy(pv, cur, (size_t)out_w * 2);
                    dirty_rows++;
                }
            }
            frames++; fps_count++;
            acc_rows += dirty_rows;
            if (max_delta > acc_maxd) acc_maxd = max_delta;
        }
        clock_gettime(CLOCK_MONOTONIC, &t2);
        acc_dec_ms   += (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        acc_scale_ms += (t1a.tv_sec - t1.tv_sec) * 1000 + (t1a.tv_nsec - t1.tv_nsec) / 1000000;
        acc_blit_ms  += (t2.tv_sec - t1a.tv_sec) * 1000 + (t2.tv_nsec - t1a.tv_nsec) / 1000000;

        /* 1Hz 帧率 + 分步耗时报告 */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t_last.tv_sec) * 1000 +
                  (now.tv_nsec - t_last.tv_nsec) / 1000000;
        if (ms >= 1000) {
            DEMO_LOG("%d fps (%llu) | dec %ldms | scale %ldms | blit %ldms (%ld/%d rows, maxΔ %d)",
                     fps_count, (unsigned long long)frames,
                     fps_count ? acc_dec_ms / fps_count : 0,
                     fps_count ? acc_scale_ms / fps_count : 0,
                     fps_count ? acc_blit_ms / fps_count : 0,
                     fps_count ? acc_rows / fps_count : 0, out_h, acc_maxd);
            fps_count = 0;
            acc_dec_ms = acc_scale_ms = acc_blit_ms = acc_rows = 0;
            acc_maxd = 0;
            t_last = now;
        }
    }

    DEMO_LOG("Stopping (%llu frames displayed)", (unsigned long long)frames);
    close_camera();                  /* 停流 + 释放 buffer */
    fb_fill(0x0000);                 /* 退出前清屏 */
    close_fb();
    free(rgb);
    return 0;
#else
    /* 非 Linux host：只做编译/链接烟雾测试（与 demo_image.c / demo2.c 同款模式）。
     * 传一个 JPEG 路径可验证 libjpeg 链接路径通。 */
    (void)argc; (void)argv;
    printf("[demo_camera] non-Linux host build: skipping framebuffer/V4L2 demo.\n");
    if (argc >= 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { fprintf(stderr, "[demo_camera] open %s failed\n", argv[1]); return 1; }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
        long n = ftell(f);
        if (n < 0) { fclose(f); return 1; }
        rewind(f);
        unsigned char *buf = (unsigned char *)malloc((size_t)n);
        if (!buf) { fclose(f); return 1; }
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return 1; }
        fclose(f);
        int w = 0, h = 0;
        if (!jpeg_get_size(buf, (size_t)n, &w, &h)) {
            fprintf(stderr, "[demo_camera] JPEG header read failed: %s\n", argv[1]);
            free(buf);
            return 1;
        }
        printf("[demo_camera] JPEG header OK on host build: %dx%d\n", w, h);
        free(buf);
    }
    return 0;
#endif
}
