// csrc/screen_convert.cpp — 图像换算：RGB → RGB565（查表旋转 + 缩放）
//
// 只有纯计算（不碰 /dev/fb0）：目标缓冲区由调用方给。
// convert_box 是"一块内存 → 一块内存"的静态工具（板上刷屏与待机图都用它）；
// convert 是成员版：读最新帧、写自己的 buf_。

#include "csrc/screen_display.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if AKA_WITH_SCREEN   // 不带屏版本整个显示栈不参与编译（桩在下面）

namespace csrc {

/// 待机图转换：来源 RGB8 → 目标 RGB565，几何与 convert() 完全一致
/// （90° 顺时针旋转 + cover 缩放居中裁切 + orient 位0=水平翻/位1=垂直翻），
/// 差别只在取样方式：这里对每个目标像素取源图一小块做**盒式平均**。
/// 照片从 750x500 缩到 320x480 时，平均比最近邻干净得多（没有锯齿/摩尔纹）。
void ScreenDisplay::convert_box(const uint8_t* rgb, int w, int h, int out_w, int out_h, int orient,
                                uint16_t* dst) {
    if (!rgb || w <= 0 || h <= 0 || out_w <= 0 || out_h <= 0 || !dst) return;

    const double sw = (double)h;      // 旋转后宽
    const double sh = (double)w;      // 旋转后高
    const double s = (out_w / sw) > (out_h / sh) ? (out_w / sw) : (out_h / sh);
    if (s <= 0) return;
    const double off_x = (sw * s - out_w) / 2.0;
    const double off_y = (sh * s - out_h) / 2.0;

    // 取样块边长：缩小时 >1（做平均），放大时为 1（等价最近邻）
    int box = (int)std::ceil(1.0 / s);
    if (box < 1) box = 1;
    const int half = box / 2;
    const size_t stride = (size_t)w * 3;

    for (int oy = 0; oy < out_h; oy++) {
        // 输出行 ← 源列（与 convert 同一公式）
        const int sx_c = (int)((off_y + oy + 0.5) / s);
        int sx0 = sx_c - half, sx1 = sx_c - half + box - 1;
        if (sx0 < 0) sx0 = 0;
        if (sx1 > w - 1) sx1 = w - 1;
        const int dst_oy = (orient & 2) ? (out_h - 1 - oy) : oy;
        uint16_t* row = dst + (size_t)dst_oy * out_w;

        for (int ox = 0; ox < out_w; ox++) {
            // 输出列 ← 源行（行号随 ox 增大而减小）
            const int sy_c = (int)(h - 1 - (off_x + ox + 0.5) / s);
            int sy0 = sy_c - half, sy1 = sy_c - half + box - 1;
            if (sy0 < 0) sy0 = 0;
            if (sy1 > h - 1) sy1 = h - 1;

            uint32_t ar = 0, ag = 0, ab = 0, n = 0;
            for (int sy = sy0; sy <= sy1; sy++) {
                const uint8_t* src = rgb + (size_t)sy * stride;
                for (int sx = sx0; sx <= sx1; sx++) {
                    const uint8_t* px = src + (size_t)sx * 3;
                    ar += px[0];
                    ag += px[1];
                    ab += px[2];
                    n++;
                }
            }
            const int dst_ox = (orient & 1) ? (out_w - 1 - ox) : ox;
            if (n == 0) {
                row[dst_ox] = 0;
            } else {
                const uint8_t r = (uint8_t)(ar / n), g = (uint8_t)(ag / n), b = (uint8_t)(ab / n);
                row[dst_ox] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }
}

#if defined(__linux__)

/// 源 w x h 的 RGB8（行宽 w*3）；旋转后尺寸为 h x w。
/// 逐像素反推：
///   输出列 ox → 源行 sy（越靠右=源越靠上），输出行 oy → 源列 sx。
/// 反推映射只依赖 ox 或 oy，预先算成整数查表；riscv64 软浮点下逐像素除法极慢，
/// 查表后内层只剩整数运算（板上实测这一步 ~7ms/帧）。
void ScreenDisplay::convert(const uint8_t* rgb, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const double sw = (double)h;      // 旋转后宽
    const double sh = (double)w;      // 旋转后高
    const double s = (out_w_ / sw) > (out_h_ / sh) ? (out_w_ / sw) : (out_h_ / sh);
    const double off_x = (sw * s - out_w_) / 2.0;
    const double off_y = (sh * s - out_h_) / 2.0;
    const size_t stride = (size_t)w * 3;

    for (int ox = 0; ox < out_w_; ++ox) {
        int sy = (int)(h - 1 - (off_x + ox) / s + 0.5);
        if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;
        sy_map_[ox] = sy;
    }
    for (int oy = 0; oy < out_h_; ++oy) {
        int sx = (int)((off_y + oy) / s + 0.5);
        if (sx < 0) sx = 0; else if (sx >= w) sx = w - 1;
        sx_map_[oy] = sx;
    }

    for (int oy = 0; oy < out_h_; ++oy) {
        const size_t col_off = (size_t)sx_map_[oy] * 3;   // 源列字节偏移
        // 方向修正：位0=水平翻转(ox)，位1=垂直翻转(oy)
        const int dst_oy = (cfg_.orient & 2) ? (out_h_ - 1 - oy) : oy;
        uint16_t* dst = buf_.data() + (size_t)dst_oy * out_w_;
        for (int ox = 0; ox < out_w_; ++ox) {
            const int dst_ox = (cfg_.orient & 1) ? (out_w_ - 1 - ox) : ox;
            const uint8_t* p = rgb + (size_t)sy_map_[ox] * stride + col_off;
            dst[dst_ox] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
}

/// 脏行写屏：只把内容变化的行写进 fb0（屏按行扫描，静态区域完全不重写）。
/// 行间必须按屏幕行宽 fb_w_ 跳，不能整块连续拷贝（否则图像在屏上斜着拼）。

#else  // !__linux__ —— 开发机 stub

void ScreenDisplay::convert(const uint8_t*, int, int) {}

#endif  // __linux__

}  // namespace csrc

#else  // !AKA_WITH_SCREEN

namespace csrc {

void ScreenDisplay::convert_box(const uint8_t*, int, int, int, int, int, uint16_t*) {}

void ScreenDisplay::convert(const uint8_t*, int, int) {}

}  // namespace csrc

#endif  // AKA_WITH_SCREEN
