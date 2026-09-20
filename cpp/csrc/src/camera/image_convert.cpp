// csrc/camera/image_convert.cpp — 图像换算：JPEG ↔ RGB / YUYV / letterbox
//
// 纯函数，不碰设备（采集循环在 camera.cpp）。libjpeg 的错误处理用 setjmp 兜住
// —— libjpeg 默认会直接 exit()，板上那样等于整个服务没了。

#include "csrc/camera.hpp"

#include <algorithm>
#include <cstring>

#include "csrc/log.hpp"

#if defined(__linux__)

#include <jpeglib.h>
#include <setjmp.h>

namespace {

struct JpegErr {
    jpeg_error_mgr pub;
    jmp_buf jump;
};
// 全局解码锁：这个 libjpeg 构建（IJG jpeg-9f，交叉编译）在**两个线程同时解码**时会
// 把内存搞坏 —— 板上实测：屏显示线程（8fps 解码）与脚本线程（检测解码）并发时，会在
// jpeg_idct_* 里以 badaddr≈0x46 段错误崩掉；而只让单个解码者跑（纯 HTTP 线程连打
// 120 次）则完全正常。解码本来是十几毫秒的一次性开销，串行化代价可以接受，
// 换来的是"随便几个消费者都不会互相踩"。
std::mutex g_jpeg_mu;

void jpeg_on_error(j_common_ptr cinfo) {
    char buf[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buf);
    CAM_WARN("JPEG error: %s", buf);
    longjmp(((JpegErr*)cinfo->err)->jump, 1);
}
}  // namespace

namespace csrc {

// ── JPEG 工具 ──

bool Camera::jpeg_to_rgb(const uint8_t* jpg, size_t len, int& w, int& h,
                         std::vector<uint8_t>& rgb, int max_out_w) {
    std::lock_guard<std::mutex> lk(g_jpeg_mu);   // 见 jpeg_to_rgb 上方 g_jpeg_mu 的说明
    jpeg_decompress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    // 解码缓冲**在 setjmp 之前**就建好（此刻还不知道尺寸，先给个空的）：
    // libjpeg 出错走 longjmp，而 longjmp 不析构 C++ 对象 —— 缓冲要是声明在 setjmp
    // 之后，一旦在 jpeg_read_scanlines 里撞上 error_exit，这块 w*h*3（640×480 约
    // 900KB）就每张坏帧漏一次。声明在 setjmp 之前的对象在 longjmp 后依然有效、
    // 沿 setjmp 那条 return 正常析构，所以这里只把 unique_ptr 本身提到前面。
    auto buf = std::make_unique<std::vector<uint8_t>>();
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpg, len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_RGB;
    unsigned scale = 1;
    if (max_out_w > 0) {
        while ((cinfo.image_width / scale) > (unsigned)max_out_w && scale < 8) scale <<= 1;
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = scale;
    jpeg_start_decompress(&cinfo);
    w = (int)cinfo.output_width;
    h = (int)cinfo.output_height;
    // 尺寸兜底：0 或离谱的尺寸直接判失败。以前这里不查，`rgb.resize(0)` 之后
    // `&rgb[0]` 是 nullptr，而 libjpeg 照样往那一行写 —— 表现为在 jpeg_idct_* 里
    // 收到 badaddr≈0x46 的段错误（板上实测）。宁可当坏帧丢掉。
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        CAM_WARN("[cam] 解码尺寸异常 %dx%d，按坏帧丢弃", w, h);
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    // 先解到独立缓冲，全部成功后再交给调用方：中途失败不会留下半张图
    buf->resize((size_t)w * h * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* row = buf->data() + (size_t)cinfo.output_scanline * w * 3;
        const JDIMENSION got = jpeg_read_scanlines(&cinfo, (JSAMPARRAY)&row, 1);
        if (got == 0) {          // 读不动了（截断/finish 不了）→ 别原地死循环
            CAM_WARN("[cam] 解码中断@行 %u/%u，按坏帧丢弃", cinfo.output_scanline,
                     cinfo.output_height);
            jpeg_abort_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);
            return false;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    rgb = std::move(*buf);
    return true;
}

bool Camera::rgb_to_jpeg(const uint8_t* rgb, int w, int h, int quality,
                         std::vector<uint8_t>& out) {
    jpeg_compress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_compress(&cinfo);
        return false;
    }
    jpeg_create_compress(&cinfo);

    unsigned char* mem = nullptr;
    unsigned long mem_len = 0;
    jpeg_mem_dest(&cinfo, &mem, &mem_len);

    cinfo.image_width = (JDIMENSION)w;
    cinfo.image_height = (JDIMENSION)h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + (size_t)cinfo.next_scanline * w * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    out.assign(mem, mem + mem_len);
    free(mem);
    return true;
}

void Camera::yuyv_to_rgb(const uint8_t* src, int w, int h, uint8_t* rgb) {
    for (int sy = 0; sy < h; ++sy) {
        for (int sx = 0; sx < w; ++sx) {
            const uint8_t* py = src + ((size_t)sy * w + sx) * 2;
            const uint8_t* pc = src + ((size_t)sy * w + (sx & ~1)) * 2;
            uint8_t Y = py[0], U = pc[1], V = pc[3];
            int C = (int)Y - 16, D = (int)U - 128, E = (int)V - 128;
            int r = (298 * C + 409 * E + 128) >> 8;
            int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int b = (298 * C + 516 * D + 128) >> 8;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            rgb[(size_t)sy * w * 3 + sx * 3 + 0] = (uint8_t)r;
            rgb[(size_t)sy * w * 3 + sx * 3 + 1] = (uint8_t)g;
            rgb[(size_t)sy * w * 3 + sx * 3 + 2] = (uint8_t)b;
        }
    }
}

bool Camera::letterbox_rgb(const uint8_t* rgb, int w, int h,
                           uint8_t* out, int out_w, int out_h) {
    if (w <= 0 || h <= 0 || out_w <= 0 || out_h <= 0) return false;
    double r = std::min((double)out_w / w, (double)out_h / h);
    int new_w = (int)(w * r + 0.5);
    int new_h = (int)(h * r + 0.5);
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;
    int dw = (out_w - new_w) / 2;
    int dh = (out_h - new_h) / 2;
    std::memset(out, 0, (size_t)out_w * out_h * 3);  // 黑边

    for (int y = 0; y < new_h; y++) {
        int sy = (int)(y / r);
        if (sy >= h) sy = h - 1;
        const uint8_t* src_row = rgb + (size_t)sy * w * 3;
        uint8_t* dst_row = out + ((size_t)(dh + y) * out_w + dw) * 3;
        for (int x = 0; x < new_w; x++) {
            int sx = (int)(x / r);
            if (sx >= w) sx = w - 1;
            const uint8_t* p = src_row + (size_t)sx * 3;
            dst_row[x * 3 + 0] = p[0];
            dst_row[x * 3 + 1] = p[1];
            dst_row[x * 3 + 2] = p[2];
        }
    }
    return true;
}

bool Camera::jpeg_get_size(const uint8_t* jpg, size_t len, int& w, int& h) {
    jpeg_decompress_struct cinfo;
    JpegErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpg, len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    w = (int)cinfo.image_width;
    h = (int)cinfo.image_height;
    jpeg_destroy_decompress(&cinfo);
    return true;
}

}  // namespace csrc

#else  // !__linux__ —— 开发机 stub

namespace csrc {

bool Camera::jpeg_to_rgb(const uint8_t*, size_t, int&, int&, std::vector<uint8_t>&, int) {
    return false;
}

bool Camera::rgb_to_jpeg(const uint8_t*, int, int, int, std::vector<uint8_t>&) {
    return false;
}

void Camera::yuyv_to_rgb(const uint8_t*, int, int, uint8_t*) {}

bool Camera::letterbox_rgb(const uint8_t*, int, int, uint8_t*, int, int) { return false; }

bool Camera::jpeg_get_size(const uint8_t*, size_t, int&, int&) { return false; }

}  // namespace csrc

#endif  // __linux__
