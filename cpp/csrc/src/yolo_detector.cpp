// csrc/yolo_detector.cpp — cviruntime 边界层实现
//
// 板上换来的几条硬规矩（少一条就"能编译、能加载、但一个框都没有"）：
//   1. 输入格式从张量报的 pixel_format 分派，**绝不假设 RGB** —— 模型报 YUV 时喂
//      平面 RGB，分数会整体偏低，最后表现为 count=0。
//   2. 输入/输出的行距必须用 mem_size 推（张量按行对齐，多出来的 padding 不跳过
//      会让整幅图错位，所有框一起偏）。
//   3. 输出形状用 derive_output_layout() 推（详见 yolo.cpp 里的说明）。
//   4. load() 成功时打一行规格日志 —— 这一个 printf 能顶掉一半现场排查。

#include "csrc/yolo_detector.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

#if AKA_WITH_TPU

#include "cviruntime.h"   // -I$(TPU_SDK_DIR)/include（不 vendor 进仓库）

#include "csrc/log.hpp"

namespace csrc {

namespace {

/// 张量"行"的总数（用于从 mem_size 反推行距）
size_t rows_total(PixFmt f, int h) {
    switch (f) {
        case PixFmt::RGB_PLANAR:
        case PixFmt::BGR_PLANAR:
            return (size_t)h * 3;                     // 三个通道各 h 行
        case PixFmt::YUV_NV12:
        case PixFmt::YUV_NV21:
        case PixFmt::YUV420_PLANAR:
            return (size_t)h + (size_t)h / 2;         // Y 的 h 行 + 色度 h/2 行
        default:
            return (size_t)h;
    }
}

/// 每"行"的紧凑字节数（不含行对齐 padding）
size_t compact_row(PixFmt f, int w, int esz) {
    switch (f) {
        case PixFmt::RGB_PACKED:
        case PixFmt::BGR_PACKED:
            return (size_t)w * 3 * esz;
        case PixFmt::YUV_NV12:
        case PixFmt::YUV_NV21:
        case PixFmt::YUV420_PLANAR:
            return (size_t)w;                        // YUV 按 8 位
        default:
            return (size_t)w * esz;                  // 平面 RGB / 灰度
    }
}

PixFmt map_pixel_format(CVI_NN_PIXEL_FORMAT_E pf, const CVI_SHAPE& shp, bool& is_tensor) {
    is_tensor = false;
    switch (pf) {
        case CVI_NN_PIXEL_RGB_PLANAR:     return PixFmt::RGB_PLANAR;
        case CVI_NN_PIXEL_BGR_PLANAR:     return PixFmt::BGR_PLANAR;
        case CVI_NN_PIXEL_RGB_PACKED:     return PixFmt::RGB_PACKED;
        case CVI_NN_PIXEL_BGR_PACKED:     return PixFmt::BGR_PACKED;
        case CVI_NN_PIXEL_YUV_NV12:       return PixFmt::YUV_NV12;
        case CVI_NN_PIXEL_YUV_NV21:       return PixFmt::YUV_NV21;
        case CVI_NN_PIXEL_YUV_420_PLANAR: return PixFmt::YUV420_PLANAR;
        case CVI_NN_PIXEL_GRAYSCALE:      return PixFmt::GRAYSCALE;
        case CVI_NN_PIXEL_TENSOR:
            // 纯张量：没有图像语义，通道顺序由模型自己的预处理节点决定。
            // 这里按布局当 RGB 处理（唯一能做的合理猜测），日志里会标出来。
            is_tensor = true;
            if (shp.dim_size >= 4 && shp.dim[1] == 3) return PixFmt::RGB_PLANAR;
            return PixFmt::RGB_PACKED;
        default:
            return PixFmt::UNKNOWN;
    }
}

size_t elem_size_of(CVI_FMT fmt) {
    switch (fmt) {
        case CVI_FMT_INT8:
        case CVI_FMT_UINT8:  return 1;
        case CVI_FMT_INT16:
        case CVI_FMT_UINT16:
        case CVI_FMT_BF16:   return 2;
        default:             return 4;
    }
}

}  // namespace

YoloDetector::~YoloDetector() { close(); }

void YoloDetector::close() {
    if (model_) {
        CVI_NN_CleanupModel((CVI_MODEL_HANDLE)model_);
        model_ = nullptr;
    }
    input_ = output_ = nullptr;
    in_num_ = out_num_ = 0;
    loaded_ = false;
    path_.clear();
    info_.clear();
    lb_buf_.clear();
    scratch_.clear();
}

bool YoloDetector::load(const std::string& path, std::string& err) {
    close();
    if (path.empty()) {
        err = "模型路径为空";
        return false;
    }

    CVI_MODEL_HANDLE model = nullptr;
    CVI_RC rc = CVI_NN_RegisterModel(path.c_str(), &model);
    if (rc != 0 || !model) {
        err = "注册模型失败（CVI_NN_RegisterModel rc=" + std::to_string(rc) + "）：" + path;
        return false;
    }
    model_ = model;
    path_ = path;

    CVI_TENSOR* inputs = nullptr;
    CVI_TENSOR* outputs = nullptr;
    int32_t in_num = 0, out_num = 0;
    rc = CVI_NN_GetInputOutputTensors(model, &inputs, &in_num, &outputs, &out_num);
    if (rc != 0 || !inputs || in_num < 1 || !outputs || out_num < 1) {
        err = "取张量失败（rc=" + std::to_string(rc) + "，输入 " + std::to_string(in_num) +
              " 个，输出 " + std::to_string(out_num) + " 个）";
        close();
        return false;
    }
    input_ = CVI_NN_GetTensorByName(CVI_NN_DEFAULT_TENSOR, inputs, in_num);
    if (!input_) {
        err = "取默认输入张量失败";
        close();
        return false;
    }
    output_ = &outputs[0];
    in_num_ = 1;
    out_num_ = out_num;
    if (out_num > 1) {
        // 多输出的模型没实现：只取 outputs[0]，其余忽略。宁可日志里说清楚，
        // 也不要静默用错输出。
        CAM_WARN("[detect] %s 有 %d 个输出，只用 outputs[0]（多输出未实现）", path.c_str(),
                 out_num);
    }

    // ── 输入尺寸 / 布局：从张量形状推，不从配置读 ──
    CVI_TENSOR* in = (CVI_TENSOR*)input_;
    CVI_SHAPE shp = CVI_NN_TensorShape(in);
    int iw = 0, ih = 0;
    if (shp.dim_size >= 4) {
        if (shp.dim[1] == 3) {              // NCHW
            ih = shp.dim[shp.dim_size - 2];
            iw = shp.dim[shp.dim_size - 1];
        } else if (shp.dim[shp.dim_size - 1] == 3) {   // NHWC
            ih = shp.dim[1];
            iw = shp.dim[2];
        }
    } else if (shp.dim_size == 3) {         // [3,H,W]
        ih = shp.dim[1];
        iw = shp.dim[2];
    }
    if (iw <= 0 || ih <= 0) {
        err = "输入形状看不懂：" + shape_str(shp.dim, shp.dim_size);
        close();
        return false;
    }

    bool is_tensor = false;
    const PixFmt fmt = map_pixel_format(in->pixel_format, shp, is_tensor);
    if (fmt == PixFmt::UNKNOWN) {
        err = "不支持的输入 pixel_format（id=" + std::to_string((int)in->pixel_format) + "）";
        close();
        return false;
    }

    InputSpec spec;
    spec.w = iw;
    spec.h = ih;
    spec.fmt = fmt;
    switch (in->fmt) {
        case CVI_FMT_INT8:
        case CVI_FMT_UINT8:
            spec.elem_size = 1;
            spec.one_byte = true;          // 1 字节：原样搬（量化烧在模型里）
            break;
        case CVI_FMT_FP32:
            spec.elem_size = 4;
            spec.one_byte = false;         // 每通道 256 项查表做归一化
            break;
        case CVI_FMT_BF16:
        default:
            // 明确报错而不是静默喂错数据（BF16 输入的板上没人验过）
            err = "输入元素类型未实现（CVI_FMT=" + std::to_string((int)in->fmt) + "）";
            close();
            return false;
    }
    for (int c = 0; c < 3; c++) {
        spec.mean[c] = in->mean[c];
        spec.scale[c] = in->scale[c];
    }
    const size_t in_compact = compact_row(fmt, iw, spec.elem_size) * rows_total(fmt, ih);
    in_pitch_ = 0;
    if (in->mem_size > in_compact) {
        in_pitch_ = in->mem_size / rows_total(fmt, ih);   // 行对齐 padding
    }
    in_spec_ = spec;
    lb_buf_.resize((size_t)iw * ih * 3);

    // ── 输出布局 ──
    CVI_TENSOR* out = (CVI_TENSOR*)output_;
    CVI_SHAPE oshp = CVI_NN_TensorShape(out);
    derive_output_layout(oshp.dim, oshp.dim_size, out_channels_, out_anchors_, out_channel_major_);
    if (out_channels_ < 5 || out_anchors_ < 1) {
        err = "输出形状推导失败：" + shape_str(oshp.dim, oshp.dim_size);
        close();
        return false;
    }
    out_fmt_ = (int)out->fmt;
    out_elem_size_ = (int)elem_size_of(out->fmt);
    out_qscale_ = out->qscale;
    out_zero_point_ = out->zero_point;
    const int orows = out_channel_major_ ? out_channels_ : out_anchors_;
    const int ocols = out_channel_major_ ? out_anchors_ : out_channels_;
    const size_t o_compact = (size_t)orows * ocols * out_elem_size_;
    out_pitch_ = 0;
    if (out->mem_size > o_compact) out_pitch_ = out->mem_size / (size_t)orows;
    scratch_.resize((size_t)out_channels_ * out_anchors_);

    loaded_ = true;
    {
        // 这一行是现场排查的入口：尺寸/格式/量化参数/推导出的通道数
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "in %dx%d %s elem=%d%s pitch=%zu | out %s C=%d N=%d %s fmt=%d q=%.6f zp=%d "
                 "pitch=%zu%s",
                 iw, ih, pix_fmt_name(fmt), spec.elem_size, spec.one_byte ? "(raw)" : "(norm)",
                 in_pitch_, shape_str(oshp.dim, oshp.dim_size).c_str(), out_channels_,
                 out_anchors_, out_channel_major_ ? "[C][N]" : "[N][C]", out_fmt_, out_qscale_,
                 out_zero_point_, out_pitch_, is_tensor ? " (PIXEL_TENSOR：通道序按 RGB 猜)" : "");
        info_ = buf;
    }
    CAM_INFO("[detect] 模型已加载 %s | %s", path.c_str(), info_.c_str());
    return true;
}

void YoloDetector::read_output() {
    CVI_TENSOR* out = (CVI_TENSOR*)output_;
    const uint8_t* base = (const uint8_t*)CVI_NN_TensorPtr(out);
    const int rows = out_channel_major_ ? out_channels_ : out_anchors_;
    const int cols = out_channel_major_ ? out_anchors_ : out_channels_;
    const size_t row_bytes =
        out_pitch_ ? out_pitch_ : (size_t)cols * (size_t)out_elem_size_;
    scratch_.resize((size_t)out_channels_ * out_anchors_);
    for (int r = 0; r < rows; r++) {
        const uint8_t* rp = base + (size_t)r * row_bytes;
        float* dp = scratch_.data() + (size_t)r * cols;
        switch (out_fmt_) {
            case CVI_FMT_FP32: {
                std::memcpy(dp, rp, (size_t)cols * sizeof(float));
                break;
            }
            case CVI_FMT_INT8: {
                const int8_t* q = (const int8_t*)rp;
                for (int c = 0; c < cols; c++) dp[c] = ((float)q[c] - (float)out_zero_point_) * out_qscale_;
                break;
            }
            case CVI_FMT_UINT8: {
                const uint8_t* q = rp;
                for (int c = 0; c < cols; c++) dp[c] = ((float)q[c] - (float)out_zero_point_) * out_qscale_;
                break;
            }
            case CVI_FMT_BF16: {
                const uint16_t* q = (const uint16_t*)rp;
                for (int c = 0; c < cols; c++) {
                    const uint32_t bits = (uint32_t)q[c] << 16;   // BF16 就是 float 的高 16 位
                    float f = 0.f;
                    std::memcpy(&f, &bits, sizeof(f));
                    dp[c] = f;
                }
                break;
            }
            default:
                // load() 已经拦掉未知类型，这里只是兜底
                for (int c = 0; c < cols; c++) dp[c] = 0.f;
                break;
        }
    }
}

bool YoloDetector::detect(const uint8_t* rgb, int w, int h, const DecodeOptions& opt,
                          std::vector<Detection>& out, std::string& err) {
    out.clear();
    if (!loaded_) {
        err = "模型未加载";
        return false;
    }
    if (!rgb || w <= 0 || h <= 0) {
        err = "帧为空";
        return false;
    }

    // 分段计时（DBG 级）：判断"慢在哪一段"用。开 CSRC_LOG_LEVEL=debug 就能看到。
    auto t0 = std::chrono::steady_clock::now();
    auto ms_since = [](std::chrono::steady_clock::time_point& t) {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - t).count();
        t = now;
        return ms;
    };

    // 正变换与后面的反变换共用这一个 lb_（两处各算一遍缩放/填充会整体偏移）
    lb_ = make_letterbox(w, h, in_spec_.w, in_spec_.h);
    if (!letterbox_into_rgb(rgb, w, h, lb_, lb_buf_.data())) {
        err = "letterbox 失败";
        return false;
    }
    const double lb_ms = ms_since(t0);

    uint8_t* in_ptr = (uint8_t*)CVI_NN_TensorPtr((CVI_TENSOR*)input_);
    if (!fill_input_rgb(lb_buf_.data(), in_spec_, in_pitch_, in_ptr, err)) return false;
    const double fill_ms = ms_since(t0);

    const CVI_RC rc = CVI_NN_Forward((CVI_MODEL_HANDLE)model_, (CVI_TENSOR*)input_, in_num_,
                                     (CVI_TENSOR*)output_, out_num_);
    const double fwd_ms = ms_since(t0);
    if (rc != 0) {
        err = "推理失败（CVI_NN_Forward rc=" + std::to_string(rc) + "）";
        return false;
    }

    read_output();
    decode(scratch_.data(), out_channels_, out_anchors_, out_channel_major_, lb_, opt, out);
    out = nms(std::move(out), opt.iou, opt.max_det);
    const double post_ms = ms_since(t0);
    CAM_DEBUG("[detect/timing] letterbox=%.1fms fill=%.1fms forward=%.1fms post=%.1fms  (帧 %dx%d → 输入 %dx%d)",
              lb_ms, fill_ms, fwd_ms, post_ms, w, h, in_spec_.w, in_spec_.h);
    return true;
}

}  // namespace csrc

#else  // !AKA_WITH_TPU —— 开发机：桩（接口保持一样，行为是"明确不可用"）

namespace csrc {

YoloDetector::~YoloDetector() {}

void YoloDetector::close() {
    loaded_ = false;
    path_.clear();
    info_.clear();
}

bool YoloDetector::load(const std::string&, std::string& err) {
    err = "本构建未编入 TPU 支持（AKA_WITH_TPU=0）";
    return false;
}

bool YoloDetector::detect(const uint8_t*, int, int, const DecodeOptions&,
                          std::vector<Detection>& out, std::string& err) {
    out.clear();
    err = "本构建未编入 TPU 支持（AKA_WITH_TPU=0）";
    return false;
}

}  // namespace csrc

#endif  // AKA_WITH_TPU
