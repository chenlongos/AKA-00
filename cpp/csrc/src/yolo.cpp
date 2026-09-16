// csrc/yolo.cpp — YOLO 纯计算层实现（几何 / 形状推导 / 后处理 / 填张量）
//
// 这一层不 include 任何 TPU 头文件，开发机上直接可编可跑（见文件末尾的口径注释）。

#include "csrc/yolo.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace csrc {

const char* pix_fmt_name(PixFmt f) {
    switch (f) {
        case PixFmt::RGB_PLANAR:   return "RGB_PLANAR";
        case PixFmt::BGR_PLANAR:   return "BGR_PLANAR";
        case PixFmt::RGB_PACKED:   return "RGB_PACKED";
        case PixFmt::BGR_PACKED:   return "BGR_PACKED";
        case PixFmt::YUV_NV12:     return "YUV_NV12";
        case PixFmt::YUV_NV21:     return "YUV_NV21";
        case PixFmt::YUV420_PLANAR:return "YUV420_PLANAR";
        case PixFmt::GRAYSCALE:    return "GRAYSCALE";
        default:                   return "UNKNOWN";
    }
}

// ────────────────────────── letterbox ──────────────────────────

Letterbox make_letterbox(int src_w, int src_h, int dst_w, int dst_h) {
    Letterbox lb;
    lb.src_w = src_w;
    lb.src_h = src_h;
    lb.dst_w = dst_w;
    lb.dst_h = dst_h;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return lb;
    // 取整规则与 Camera::letterbox_rgb() 一致（那边给浏览器出图，这边喂模型）
    const double r = std::min((double)dst_w / src_w, (double)dst_h / src_h);
    lb.scale = (float)r;
    lb.new_w = (int)(src_w * r + 0.5);
    lb.new_h = (int)(src_h * r + 0.5);
    if (lb.new_w < 1) lb.new_w = 1;
    if (lb.new_h < 1) lb.new_h = 1;
    lb.pad_x = (dst_w - lb.new_w) / 2;
    lb.pad_y = (dst_h - lb.new_h) / 2;
    return lb;
}

void Letterbox::to_src(float x, float y, float& sx, float& sy) const {
    if (scale <= 0.f) { sx = x; sy = y; return; }
    sx = (x - (float)pad_x) / scale;
    sy = (y - (float)pad_y) / scale;
}

bool letterbox_into_rgb(const uint8_t* src, int src_w, int src_h, const Letterbox& lb,
                        uint8_t* dst) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || lb.dst_w <= 0 || lb.dst_h <= 0) return false;
    // 整幅先涂 114 灰：模型就是按这个填充色训练的，涂黑会让贴边的目标掉分
    std::memset(dst, kLetterboxPad, (size_t)lb.dst_w * lb.dst_h * 3);
    const float r = lb.scale;
    if (r <= 0.f) return false;
    for (int y = 0; y < lb.new_h; y++) {
        int sy = (int)(y / r);
        if (sy >= src_h) sy = src_h - 1;
        const uint8_t* srow = src + (size_t)sy * src_w * 3;
        uint8_t* drow = dst + ((size_t)(lb.pad_y + y) * lb.dst_w + lb.pad_x) * 3;
        for (int x = 0; x < lb.new_w; x++) {
            int sx = (int)(x / r);
            if (sx >= src_w) sx = src_w - 1;
            const uint8_t* p = srow + (size_t)sx * 3;
            drow[x * 3 + 0] = p[0];
            drow[x * 3 + 1] = p[1];
            drow[x * 3 + 2] = p[2];
        }
    }
    return true;
}

// ────────────────────────── 形状推导 ──────────────────────────

void derive_output_layout(const int32_t* dims, size_t dim_size, int& channels, int& anchors,
                          bool& channel_major) {
    channels = 0;
    anchors = 0;
    channel_major = true;
    if (!dims || dim_size == 0) return;

    // 丢掉 batch 维与所有 <=1 的维 —— 关键就是那些"尾巴上的 1"（[1,5,8400,1]）
    int d[8];
    int n = 0;
    for (size_t i = 0; i < dim_size && n < 8; i++) {
        if (i == 0 && dims[i] == 1) continue;   // batch
        if (dims[i] <= 1) continue;
        d[n++] = dims[i];
    }
    if (n < 2) return;                          // 形不成二维矩阵 → 调用方按错误处理
    const int a = d[n - 2];
    const int b = d[n - 1];
    channels = std::min(a, b);
    anchors = std::max(a, b);
    channel_major = (a <= b);                   // [C][N] → true，[N][C] → false
}

std::string shape_str(const int32_t* dims, size_t dim_size) {
    std::string s = "[";
    for (size_t i = 0; i < dim_size; i++) {
        if (i) s += ",";
        s += std::to_string(dims[i]);
    }
    s += "]";
    return s;
}

// ────────────────────────── 后处理 ──────────────────────────

float box_iou(const Box& a, const Box& b) {
    const float x1 = std::max(a.x1, b.x1), y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2), y2 = std::min(a.y2, b.y2);
    const float iw = x2 - x1, ih = y2 - y1;
    if (iw <= 0.f || ih <= 0.f) return 0.f;
    const float inter = iw * ih;
    const float area_a = std::max(0.f, a.x2 - a.x1) * std::max(0.f, a.y2 - a.y1);
    const float area_b = std::max(0.f, b.x2 - b.x1) * std::max(0.f, b.y2 - b.y1);
    const float uni = area_a + area_b - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

int decode(const float* data, int channels, int anchors, bool channel_major, const Letterbox& lb,
           const DecodeOptions& opt, std::vector<Detection>& out) {
    out.clear();
    if (!data || channels <= 0 || anchors <= 0) return 0;

    // YOLOv8：通道 0..3 = cx,cy,w,h，4.. = 每类分数（没有 objectness）
    // YOLOv5：通道 4 = objectness，5.. = 每类分数
    const int nc = channels - (opt.has_objectness ? 5 : 4);
    if (nc < 1) return 0;   // 形状推错时在这里兜底（会表现为 count=0 而不是乱框）

    auto at = [&](int c, int i) -> float {
        return channel_major ? data[(size_t)c * anchors + i] : data[(size_t)i * channels + c];
    };

    for (int i = 0; i < anchors; i++) {
        float score = 0.f;
        int cls = 0;
        if (opt.has_objectness) {
            const float obj = at(4, i);
            if (!(obj >= opt.conf)) continue;   // !(>=) 顺带挡掉 NaN
            for (int c = 0; c < nc; c++) {
                const float s = at(5 + c, i);
                if (s > score) { score = s; cls = c; }
            }
            score *= obj;
        } else {
            for (int c = 0; c < nc; c++) {
                const float s = at(4 + c, i);
                if (s > score) { score = s; cls = c; }
            }
        }
        if (!(score >= opt.conf)) continue;

        const float cx = at(0, i), cy = at(1, i), bw = at(2, i), bh = at(3, i);
        if (!(bw > 0.f) || !(bh > 0.f)) continue;   // 退化框

        float sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
        lb.to_src(cx - bw * 0.5f, cy - bh * 0.5f, sx1, sy1);
        lb.to_src(cx + bw * 0.5f, cy + bh * 0.5f, sx2, sy2);

        // 裁到原图范围内：整框落在 letterbox 填充区时会被裁成零面积，然后丢掉
        Detection d;
        d.cls = cls;
        d.score = score;
        d.box.x1 = std::min(std::max(sx1, 0.f), (float)lb.src_w);
        d.box.y1 = std::min(std::max(sy1, 0.f), (float)lb.src_h);
        d.box.x2 = std::min(std::max(sx2, 0.f), (float)lb.src_w);
        d.box.y2 = std::min(std::max(sy2, 0.f), (float)lb.src_h);
        if (d.box.x2 <= d.box.x1 || d.box.y2 <= d.box.y1) continue;

        out.push_back(d);
        if ((int)out.size() >= opt.max_det) break;
    }
    return (int)out.size();
}

std::vector<Detection> nms(std::vector<Detection> dets, float iou_thresh, int max_det) {
    std::sort(dets.begin(), dets.end(),
              [](const Detection& a, const Detection& b) { return a.score > b.score; });
    std::vector<Detection> keep;
    std::vector<char> dead(dets.size(), 0);
    for (size_t i = 0; i < dets.size(); i++) {
        if (dead[i]) continue;
        keep.push_back(dets[i]);
        if ((int)keep.size() >= max_det) break;
        // 类别内抑制：不同类的框互不影响（否则人身上的球拍会被"人"框吃掉）
        for (size_t j = i + 1; j < dets.size(); j++) {
            if (dead[j] || dets[j].cls != dets[i].cls) continue;
            if (box_iou(dets[i].box, dets[j].box) > iou_thresh) dead[j] = 1;
        }
    }
    return keep;
}

// ────────────────────────── 填输入张量 ──────────────────────────

namespace {

/// 单个通道值的写入：1 字节原样搬（量化烧在模型里），FP32 走 (v/255-mean)/scale
struct ChanWriter {
    bool one_byte = true;
    float inv_scale[3] = {1.f, 1.f, 1.f};
    float mean[3] = {0.f, 0.f, 0.f};

    void put(uint8_t* p, uint8_t v, int c) const {
        if (one_byte) {
            *p = v;
            return;
        }
        const float norm = ((float)v / 255.f - mean[c]) * inv_scale[c];
        std::memcpy(p, &norm, sizeof(float));
    }
};

}  // namespace

bool fill_input_rgb(const uint8_t* rgb, const InputSpec& spec, size_t pitch, uint8_t* dst,
                    std::string& err) {
    const int w = spec.w, h = spec.h;
    if (!rgb || !dst || w <= 0 || h <= 0) {
        err = "输入规格非法（w/h 为 0）";
        return false;
    }
    if (spec.elem_size != 1 && spec.elem_size != 4) {
        err = "输入元素大小 " + std::to_string(spec.elem_size) + " 字节不支持（只实现 1 字节与 FP32）";
        return false;
    }

    ChanWriter cw;
    cw.one_byte = spec.one_byte;
    for (int c = 0; c < 3; c++) {
        cw.mean[c] = spec.mean[c];
        cw.inv_scale[c] = (spec.scale[c] != 0.f) ? (1.f / spec.scale[c]) : 1.f;
    }
    const int esz = spec.elem_size;

    // 三通道的源索引：model 通道 c 取原图哪个字节（BGR 时反过来）
    const bool swap = (spec.fmt == PixFmt::BGR_PLANAR || spec.fmt == PixFmt::BGR_PACKED);
    auto src_of = [&](int c) { return swap ? (2 - c) : c; };

    switch (spec.fmt) {
        case PixFmt::RGB_PACKED:
        case PixFmt::BGR_PACKED: {
            const size_t row = pitch ? pitch : (size_t)w * 3 * esz;
            for (int y = 0; y < h; y++) {
                uint8_t* drow = dst + (size_t)y * row;
                for (int x = 0; x < w; x++) {
                    const uint8_t* px = rgb + ((size_t)y * w + x) * 3;
                    for (int c = 0; c < 3; c++)
                        cw.put(drow + ((size_t)x * 3 + c) * esz, px[src_of(c)], c);
                }
            }
            return true;
        }
        case PixFmt::RGB_PLANAR:
        case PixFmt::BGR_PLANAR: {
            const size_t row = pitch ? pitch : (size_t)w * esz;
            for (int c = 0; c < 3; c++) {
                const int sc = src_of(c);
                uint8_t* plane = dst + (size_t)c * h * row;
                for (int y = 0; y < h; y++) {
                    uint8_t* drow = plane + (size_t)y * row;
                    for (int x = 0; x < w; x++)
                        cw.put(drow + (size_t)x * esz, rgb[((size_t)y * w + x) * 3 + sc], c);
                }
            }
            return true;
        }
        case PixFmt::GRAYSCALE: {
            // BT.601 亮度（与下面 YUV 的 Y 同一套系数，避免两条路灰度不一致）
            const size_t row = pitch ? pitch : (size_t)w * esz;
            for (int y = 0; y < h; y++) {
                uint8_t* drow = dst + (size_t)y * row;
                for (int x = 0; x < w; x++) {
                    const uint8_t* px = rgb + ((size_t)y * w + x) * 3;
                    const int yv = (77 * px[0] + 150 * px[1] + 29 * px[2] + 128) >> 8;
                    cw.put(drow + (size_t)x * esz, (uint8_t)std::min(255, std::max(0, yv)), 0);
                }
            }
            return true;
        }
        case PixFmt::YUV_NV12:
        case PixFmt::YUV_NV21:
        case PixFmt::YUV420_PLANAR: {
            // 8 位平面，元素大小必然是 1
            if (esz != 1) {
                err = "YUV 输入按 8 位处理，但张量元素是 " + std::to_string(esz) + " 字节";
                return false;
            }
            if ((w & 1) || (h & 1)) {
                err = "YUV 输入尺寸必须是偶数（当前 " + std::to_string(w) + "x" + std::to_string(h) + "）";
                return false;
            }
            // RGB → YUV：BT.601 full range，与 JPEG 解出来的 RGB 口径一致
            //（不引入 16~235 的限幅范围，否则模型看到的亮度整体偏一点）
            const size_t y_row = pitch ? pitch : (size_t)w;
            const bool interleaved = (spec.fmt != PixFmt::YUV420_PLANAR);
            // 色度行距必须单独算：NV12/NV21 的 UV 行和 Y 行一样宽（w 字节，U/V 交替），
            // 所以行距相同；I420 的 U/V 行只有一半宽（w/2 字节），行距也得减半 ——
            // 按 Y 的行距写 U/V 会写到 2*w*h，而紧凑 I420 的 mem_size 只有 1.5*w*h，
            // 等于越界写进张量内存（这块内存是 cviruntime 的）。
            const size_t c_row = interleaved ? y_row : (pitch ? pitch / 2 : (size_t)(w / 2));
            uint8_t* const yp = dst;
            uint8_t* const up = dst + y_row * (size_t)h;              // Y 平面之后
            uint8_t* const vp = interleaved ? nullptr : up + c_row * (size_t)(h / 2);
            const bool swap_uv = (spec.fmt == PixFmt::YUV_NV21);

            for (int y = 0; y < h; y++) {
                uint8_t* yrow = yp + (size_t)y * y_row;
                for (int x = 0; x < w; x++) {
                    const uint8_t* px = rgb + ((size_t)y * w + x) * 3;
                    const int r = px[0], g = px[1], b = px[2];
                    yrow[x] = (uint8_t)std::min(
                        255, std::max(0, (77 * r + 150 * g + 29 * b + 128) >> 8));
                }
            }
            // 色度：每 2x2 取左上角那个像素（最近邻），与 resize 的取样口径一致
            for (int j = 0; j < h / 2; j++) {
                uint8_t* urow = up + (size_t)j * c_row;
                uint8_t* vrow = interleaved ? urow : (vp + (size_t)j * c_row);
                for (int i = 0; i < w / 2; i++) {
                    const int x = i * 2, y = j * 2;
                    const uint8_t* px = rgb + ((size_t)y * w + x) * 3;
                    const int r = px[0], g = px[1], b = px[2];
                    const int u = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
                    const int v = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
                    const uint8_t uc = (uint8_t)std::min(255, std::max(0, u));
                    const uint8_t vc = (uint8_t)std::min(255, std::max(0, v));
                    if (interleaved) {
                        const uint8_t first = swap_uv ? vc : uc;
                        const uint8_t second = swap_uv ? uc : vc;
                        urow[i * 2 + 0] = first;
                        urow[i * 2 + 1] = second;
                    } else {
                        urow[i] = uc;
                        vrow[i] = vc;
                    }
                }
            }
            return true;
        }
        default:
            err = std::string("不支持的输入 pixel_format：") + pix_fmt_name(spec.fmt);
            return false;
    }
}

}  // namespace csrc
