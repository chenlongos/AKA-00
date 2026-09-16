// csrc/yolo_detector.hpp — cviruntime 边界层：唯一碰 TPU 的地方
//
// 头文件刻意不 include 任何 SDK 头（张量句柄用 void* 存），这样引用它的代码
// （capp）在没装 SDK 的开发机上也能编译。真正的 cviruntime 调用全部锁在 .cpp 里，
// 并且整段包在 #if AKA_WITH_TPU 中：
//   AKA_WITH_TPU=1（交叉编译，默认）→ 真实实现
//   AKA_WITH_TPU=0（开发机）        → 桩：load() 直接返回失败
// 与 screen_display 的 AKA_WITH_SCREEN 是同一套写法（源文件永远参与编译，
// Makefile 里不用条件式源文件列表）。
//
// 线程纪律（板上）：TPU 是单实例，本类**非线程安全**（一个实例一份上下文）。
// "加载/换模型 + 推理"必须在同一把锁里串行，且别和 demo/*/tennis 同时跑（互相抢 TPU）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "csrc/yolo.hpp"

namespace csrc {

class YoloDetector {
public:
    YoloDetector() = default;
    ~YoloDetector();

    YoloDetector(const YoloDetector&) = delete;
    YoloDetector& operator=(const YoloDetector&) = delete;

    /// 注册 cvimodel 并读出输入/输出张量规格。失败时 err 里写清是哪一步。
    bool load(const std::string& path, std::string& err);
    void close();
    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }

    /// 输入 RGB8（行宽 w*3）→ 框（**原图像素坐标**，已 NMS）。
    bool detect(const uint8_t* rgb, int w, int h, const DecodeOptions& opt,
                std::vector<Detection>& out, std::string& err);

    /// 一行日志用的规格摘要（加载成功/失败时打，现场排查全靠它）
    std::string info() const { return info_; }

private:
    /// 把输出张量读成 fp32（按行距取、按 fmt 反量化）→ scratch_
    void read_output();

    void* model_ = nullptr;    // CVI_MODEL_HANDLE
    void* input_ = nullptr;    // CVI_TENSOR*
    void* output_ = nullptr;   // CVI_TENSOR*
    int in_num_ = 0;
    int out_num_ = 0;
    bool loaded_ = false;
    std::string path_;
    std::string info_;

    InputSpec in_spec_;
    size_t in_pitch_ = 0;                 // 输入行距（字节；0 = 紧凑）
    std::vector<uint8_t> lb_buf_;         // letterbox 后的 RGB 缓冲（跨帧复用，不重分配）

    int out_channels_ = 0;
    int out_anchors_ = 0;
    bool out_channel_major_ = true;
    int out_elem_size_ = 4;
    int out_fmt_ = -1;                    // CVI_FMT_*（只在 .cpp 里解释）
    float out_qscale_ = 1.f;
    int out_zero_point_ = 0;
    size_t out_pitch_ = 0;
    std::vector<float> scratch_;          // 反量化后的输出（跨帧复用）

    Letterbox lb_;
};

}  // namespace csrc
