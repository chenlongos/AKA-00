// csrc/yolo.hpp — YOLO 推理的纯计算层（几何 + 后处理），不依赖 TPU
//
// 为什么单独拆出来：这一层是"框到底对不对"的全部责任所在，而板上调一次要烧一次
// （交叉编译 → scp → 重启 → 摆拍），所以它必须能在开发机上直接跑（喂合成张量）。
// 所有碰硬件的东西（cviruntime / 张量句柄）都在 yolo_detector.hpp 里。
//
// ── 坐标口径（贯穿全文件的唯一约定，写错任何一处都表现为"框整体偏移"）──
//   · 模型输出里的 cx,cy,w,h 是**模型输入尺寸**的像素（YOLOv8 导出的输出没有归一化，
//     也不是原图像素）；
//   · 对外的 Detection::box 一律是**原图像素坐标**，由 Letterbox::to_src() 反算；
//   · 正变换（写输入张量）与反变换（算框）**必须共用同一个 Letterbox 实例** ——
//     两处各自算一遍缩放比/填充量，就会整体平移几个像素，而且很难查。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace csrc {

// ────────────────────────── 输入张量规格 ──────────────────────────

/// 输入像素格式。**由 cviruntime 报的 pixel_format 映射而来，绝不自己假设 RGB** ——
/// 模型报 YUV 却喂平面 RGB 时，分数会整体偏低、最后表现为"一个框都没有"。
enum class PixFmt {
    RGB_PLANAR,
    BGR_PLANAR,
    RGB_PACKED,
    BGR_PACKED,
    YUV_NV12,        // Y 平面 + UV 交织（U 在前）
    YUV_NV21,        // Y 平面 + UV 交织（V 在前）
    YUV420_PLANAR,   // I420：Y / U / V 三个平面
    GRAYSCALE,
    UNKNOWN,
};

const char* pix_fmt_name(PixFmt f);

/// 输入张量规格（全部来自 cviruntime 的查询结果，没有一项是我们猜的）
struct InputSpec {
    int w = 0, h = 0;            // 模型输入尺寸（从张量形状推，不从配置读）
    PixFmt fmt = PixFmt::UNKNOWN;
    int elem_size = 1;           // 1/2/4
    bool one_byte = true;        // INT8/UINT8：原样搬字节（这两颗模型的量化烧在模型里）
    // 只有非 1 字节（FP32）才用到：norm = (v/255 - mean) / scale
    float mean[3] = {0.f, 0.f, 0.f};
    float scale[3] = {1.f, 1.f, 1.f};
};

// ────────────────────────── letterbox 几何 ──────────────────────────

/// 等比缩放 + 居中填充的几何参数。取整规则与 Camera::letterbox_rgb() **逐字一致**
/// （scale=min、new=(int)(src*r+0.5)、pad 居中取整），否则两个函数的框会差 1 像素。
/// 唯一区别是填充色：那边是给浏览器出图的（黑边），这里是喂模型的（114 灰，
/// 与 ultralytics 训练口径一致）。
struct Letterbox {
    int src_w = 0, src_h = 0;    // 原图
    int dst_w = 0, dst_h = 0;    // 模型输入
    int new_w = 0, new_h = 0;    // 缩放后（不含填充）
    int pad_x = 0, pad_y = 0;
    float scale = 1.f;

    /// 模型输入坐标 → 原图像素坐标（不去掉填充就会整体偏移 pad/scale 个像素）
    void to_src(float x, float y, float& sx, float& sy) const;
};

/// 填充色：114 灰（ultralytics letterbox 口径，模型就是按这个训练的）
constexpr uint8_t kLetterboxPad = 114;

Letterbox make_letterbox(int src_w, int src_h, int dst_w, int dst_h);

/// 等比缩放 + 居中填充到 dst_w×dst_h（填充 114 灰），输出紧凑 RGB8。
/// 只做几何，不做通道排布；排布交给 fill_input_rgb()。
bool letterbox_into_rgb(const uint8_t* src, int src_w, int src_h, const Letterbox& lb,
                        uint8_t* dst);

// ────────────────────────── 输出形状推导 ──────────────────────────

/// 从输出张量的形状推 (通道数, anchor 数)。
///
/// 规则：**先丢掉 batch 维与所有 <=1 的维，剩下取最后两维，小的当通道、大的当 anchor**。
/// 这条规则是被一个真 bug 换来的：只看最后两维时，实际输出 [1,5,8400,1] 会算出
/// channels=min(8400,1)=1 → nc = 1-4 = -3 → decode() 把所有框都丢掉，现象是
/// {"ok":true,"count":0}（"一个框都没有"）。
/// channel_major=true 表示数据是 [C][N]（YOLOv8 导出的常见布局），false 是 [N][C]。
void derive_output_layout(const int32_t* dims, size_t dim_size, int& channels, int& anchors,
                          bool& channel_major);

/// 把 dims 打印成 "[1,5,8400,1]" 形式（日志/报错用）
std::string shape_str(const int32_t* dims, size_t dim_size);

// ────────────────────────── 后处理 ──────────────────────────

struct Box {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

struct Detection {
    int cls = 0;
    float score = 0.f;
    Box box;                     // 原图像素坐标
};

struct DecodeOptions {
    float conf = 0.25f;
    float iou = 0.45f;
    bool has_objectness = false; // YOLOv5 风格（4+1+nc）才置真；YOLOv8 没有 objectness
    int max_det = 300;           // 送进 NMS 前的护栏，防坏模型把内存吃光
};

/// 解码一张输出张量 → 原图像素坐标的框（已过滤阈值、已反算坐标、未做 NMS）。
/// data 是 fp32 视图；通道顺序由 channel_major 决定。
/// 返回写入 out 的个数（0 是合法结果 —— 画面里确实没有目标）。
int decode(const float* data, int channels, int anchors, bool channel_major, const Letterbox& lb,
           const DecodeOptions& opt, std::vector<Detection>& out);

/// 类别内 NMS（按分数降序贪心），返回保留的结果
std::vector<Detection> nms(std::vector<Detection> dets, float iou_thresh, int max_det);

float box_iou(const Box& a, const Box& b);

// ────────────────────────── 填输入张量 ──────────────────────────

/// 把"已经是模型输入尺寸"的 RGB 缓冲按规格写进输入张量。
/// pitch = 字节行距（0 表示紧凑排列）；张量内部可能按行对齐，必须用调用方从
/// mem_size 推出来的 pitch，用 w*通道数 硬算会让整幅图错位、所有框都偏。
/// 失败时填 err（例如 BF16 输入：明确报错，不静默喂错数据）。
bool fill_input_rgb(const uint8_t* rgb, const InputSpec& spec, size_t pitch, uint8_t* dst,
                    std::string& err);

}  // namespace csrc
