/// dora C++ 显示屏节点（SG2002 / ST7796S 320x480 RGB565）
///
/// 取代 Rust screen-node：
///   - 订阅 `camera/image`（相机输出 MJPEG；web-server 直通，这里解 JPEG 显示）
///   - JPEG → RGB8 → 旋转 90° + fill → RGB565，mmap 写 /dev/fb0
///   - 启动时开启显示引擎（/sys/class/graphics/fb0/state=1, blank=0）
///
/// 输入格式：优先 MJPEG（libjpeg 解码）；兼容检测到 YUYV 头部时走 YUYV→RGB。
/// 颜色修复：YUYV 4:2:2 每 2 像素共享 U/V，必须从宏像素取，否则奇数像素 U/V 反。
/// 几何修复：旋转后宽 = h、高 = w，oy 映射到垂直方向（0..w），否则 oy 越界 → 下半失效。
///
/// 编译（交叉）：由 build_release.sh 的 Makefile 完成，链接 libdora_c_ffi.a + libjpeg.a。

#include "node_api.h"
#include "log.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <jpeglib.h>
#include <setjmp.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr int SW = 320;   // 屏幕宽
constexpr int SH = 480;   // 屏幕高
}  // namespace

/// 帧缓冲封装：open /dev/fb0 → FBIOGET_VSCREENINFO → mmap RGB565 写屏
struct Framebuffer {
    uint16_t* base = nullptr;
    size_t    words = 0;
    size_t    bytes = 0;
    int       fd = -1;

    bool open() {
        fd = ::open("/dev/fb0", O_RDWR);
        if (fd < 0) {
            CAM_ERROR("open /dev/fb0 failed: %s", std::strerror(errno));
            return false;
        }

        // FBIOGET_VSCREENINFO = 0x4600 (linux/fb.h 标准值)
        unsigned long request = 0x4600;
        struct FbVar { unsigned xres, yres, xres_v, yres_v, xoff, yoff, bpp; } vi;
        std::memset(&vi, 0, sizeof vi);
        if (::ioctl(fd, request, &vi) == 0 && vi.xres > 0 && vi.bpp > 0) {
            words = (size_t)vi.xres * vi.yres;
            bytes = words * (vi.bpp / 8);
        } else {
            CAM_WARN("FBIOGET_VSCREENINFO failed, assuming %dx%d @16bpp", SW, SH);
            words = SW * SH;
            bytes = words * 2;
        }

        void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            CAM_ERROR("mmap /dev/fb0 failed: %s", std::strerror(errno));
            ::close(fd); fd = -1;
            return false;
        }
        base = (uint16_t*)p;
        CAM_INFO("fb0 mapped %zu bytes (%zu px)", bytes, words);
        return true;
    }

    void unmap() {
        if (base) { ::munmap(base, bytes); base = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
    }

    void blit(const uint16_t* px, size_t n) {
        if (!base) return;
        size_t nn = n < words ? n : words;
        std::memcpy(base, px, nn * sizeof(uint16_t));
    }
} g_fb;

/// 开启显示引擎（demo2.c 要求 state=1 才显示）
static void enable_display() {
    FILE* s = fopen("/sys/class/graphics/fb0/state", "w");
    if (s) { fputs("1", s); fclose(s); }
    FILE* b = fopen("/sys/class/graphics/fb0/blank", "w");
    if (b) { fputs("0", b); fclose(b); }
    CAM_INFO("display engine enabled (state=1, blank=0)");
}

/// YUYV(4:2:2) → RGB565。src 为 w x h 的 YUYV 帧（每像素 2 字节）。
/// 旋转 90°（顺时针）+ fill 拉伸到 SWxSH 竖屏 —— 对任意 RGB8 源做最近邻采样（逆映射）。
///
/// RGB8 布局：每像素 3 字节 (r g b)，行宽 = w*3。src 为 w x h 的 RGB 帧。
/// 旋转后图像尺寸：宽 = h（源高），高 = w（源宽）。输出竖屏 (ox,oy) 逐像素反推
/// 到源。关键：oy 对应旋转后"垂直"方向（范围 0..w），ox 对应"水平"方向（0..h），
/// 且都在各自范围内居中裁切 —— 之前版本把 oy 误配到源列态，oy 越过 h 后全被
/// clamp 到最右列，导致下半画面失效。
static void rgb_to_fb(const uint8_t* rgb, int w, int h,
                      uint16_t* out, int out_w, int out_h) {
    const double sw = (double)h;      // 旋转后宽
    const double sh = (double)w;      // 旋转后高
    const double s  = (out_w / sw) > (out_h / sh) ? (out_w / sw) : (out_h / sh);
    const double off_x = (sw * s - out_w) / 2.0;   // 水平居中裁切
    const double off_y = (sh * s - out_h) / 2.0;   // 垂直居中裁切
    const size_t stride = (size_t)w * 3;

    for (int oy = 0; oy < out_h; ++oy) {
        // 旋转后 y（垂直，沿输出行方向），范围 0..sh(=w)，对应源列 sx
        double rot_y = (off_y + oy) / s;
        int sx = (int)(rot_y + 0.5);
        if (sx < 0) sx = 0; else if (sx >= w) sx = w - 1;

        for (int ox = 0; ox < out_w; ++ox) {
            // 旋转后 x（水平，沿输出列方向），范围 0..sw(=h)，翻转后对应源行 sy
            double rot_x = (off_x + ox) / s;
            int sy = (int)(h - 1 - rot_x + 0.5);
            if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;

            const uint8_t* p = rgb + (size_t)sy * stride + (size_t)sx * 3;
            int r = p[0], g = p[1], b = p[2];
            out[(size_t)oy * out_w + ox] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

/// YUYV(4:2:2) → RGB8（兼容旧输入）。src 为 w x h 的 YUYV 帧（每像素 2 字节）。
/// 4:2:2 一行字节流：Y0 U0 Y1 V0 Y2 U2 Y3 V2 ...，每 2 像素共享一对 U/V。
/// Y 是每像素自己的，U/V 必须从宏像素起点 (sx & ~1) 取 —— 否则奇数像素 U/V 反。
static void yuyv_to_rgb(const uint8_t* src, int w, int h, uint8_t* rgb) {
    for (int sy = 0; sy < h; ++sy) {
        for (int sx = 0; sx < w; ++sx) {
            const uint8_t* py = src + ((size_t)sy * w + sx) * 2;
            const uint8_t* pc = src + ((size_t)sy * w + (sx & ~1)) * 2;
            uint8_t Y = py[0], U = pc[1], V = pc[3];
            int C = (int)Y - 16, D = (int)U - 128, E = (int)V - 128;
            int r = (298*C + 409*E + 128) >> 8;
            int g = (298*C - 100*D - 208*E + 128) >> 8;
            int b = (298*C + 516*D + 128) >> 8;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            rgb[(size_t)sy * w * 3 + sx * 3 + 0] = (uint8_t)r;
            rgb[(size_t)sy * w * 3 + sx * 3 + 1] = (uint8_t)g;
            rgb[(size_t)sy * w * 3 + sx * 3 + 2] = (uint8_t)b;
        }
    }
}

// libjpeg 错误桩：setjmp/longjmp 跳出解码。
// jpeg_error_mgr 必须是结构体首成员，这样 cinfo->err 能转型回 JpegErr* 拿 jmp_buf。
struct JpegErr { jpeg_error_mgr pub; jmp_buf jump; };
static void jpeg_on_error(j_common_ptr cinfo) {
    char buf[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buf);
    CAM_WARN("JPEG decode error: %s", buf);
    longjmp(((JpegErr*)cinfo->err)->jump, 1);
}

/// 解码 JPEG 字节 → RGB8（w*h*3），返回 true。w/h 为实际解码尺寸。
static bool jpeg_to_rgb(const uint8_t* jpg, size_t len, int* w, int* h,
                        std::vector<uint8_t>& rgb_out) {
    jpeg_decompress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);   // 关联错误桩
    jerr.pub.error_exit = jpeg_on_error;     // 覆盖错误回调
    if (setjmp(jerr.jump)) {                 // 出错时 longjmp 回来
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpg, len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    *w = (int)cinfo.image_width;
    *h = (int)cinfo.image_height;
    jpeg_start_decompress(&cinfo);

    rgb_out.resize((size_t)(*w) * (*h) * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* row = &rgb_out[(size_t)cinfo.output_scanline * (*w) * 3];
        jpeg_read_scanlines(&cinfo, (JSAMPARRAY)&row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

/// 判断字节流是否为 JPEG（SOI 标记 FF D8）。
static bool is_jpeg(const uint8_t* p, size_t n) {
    return n >= 2 && p[0] == 0xFF && p[1] == 0xD8;
}

int main() {
    CAM_INFO("Starting (display node)");

    void* ctx = init_dora_context_from_env();
    if (!ctx) { CAM_ERROR("Failed to init dora context"); return 1; }

    enable_display();
    if (!g_fb.open()) {
        CAM_ERROR("no /dev/fb0 — running without display");
    }

    std::vector<uint16_t> out(SW * SH, 0);
    std::vector<uint8_t>  rgb;        // 解码出的 RGB8 帧（JPEG/YUYV 共用）
    uint64_t frames = 0;
    auto t_last = std::chrono::steady_clock::now();
    int fps_count = 0;

    while (true) {
        void* event = dora_next_event(ctx);
        if (!event) continue;
        int type = read_dora_event_type(event);
        if (type == DoraEventType_Stop) {
            CAM_INFO("Stop, exiting (%llu frames)", (unsigned long long)frames);
            free_dora_event(event); break;
        }
        if (type == DoraEventType_Input) {
            char* id_ptr = nullptr; size_t id_len = 0;
            read_dora_input_id(event, &id_ptr, &id_len);
            if (!id_ptr) { free_dora_event(event); continue; }
            std::string id(id_ptr, id_len);
            if (id == "image") {
                char* data_ptr = nullptr; size_t data_len = 0;
                read_dora_input_data(event, &data_ptr, &data_len);
                if (data_ptr && data_len > 0) {
                    const auto* src = (const uint8_t*)data_ptr;
                    int w = 0, h = 0;
                    if (is_jpeg(src, data_len)) {
                        // MJPEG：libjpeg 解到 RGB8，再旋转写屏
                        if (!jpeg_to_rgb(src, data_len, &w, &h, rgb)) continue;
                    } else if (data_len >= (size_t)640 * 360 * 2) {
                        // YUYV 兼容（旧输入）：640x360
                        w = 640; h = 360;
                        rgb.resize((size_t)w * h * 3);
                        yuyv_to_rgb(src, w, h, rgb.data());
                    } else {
                        continue;
                    }
                    if (w > 0 && h > 0) {
                        rgb_to_fb(rgb.data(), w, h, out.data(), SW, SH);
                        g_fb.blit(out.data(), out.size());
                        frames++; fps_count++;
                    }
                }
            }
        }
        free_dora_event(event);

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_last).count();
        if (ms >= 1000) { CAM_DEBUG("%d fps", fps_count); fps_count = 0; t_last = std::chrono::steady_clock::now(); }
    }

    g_fb.unmap();
    free_dora_context(ctx);
    return 0;
}