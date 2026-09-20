// 单帧推理服务
//
// 由 capp/src/services.cpp 按域拆出来（对应 app/services/*.py 的分法）。
// 取当前帧跑一次模型（GET /api/detect 与脚本的 detect() 共用）。
// 声明都在 capp/context.hpp（那一份是按域分节的伞头文件，调用方只 include 它）。

#include "capp/context.hpp"

#include "csrc/camera.hpp"
#include "csrc/log.hpp"
#include <algorithm>
#include <chrono>
#include <sys/stat.h>

namespace capp {

/// 取一帧跑一次推理 → 框列表（原图像素坐标）。`/api/detect` 与脚本原语共用同一条链：
/// 模型懒加载 / 文件变了重载 / 取原生帧 / 推理。错误串与对外契约保持一致。
bool detect_boxes(AppContext& ctx, const std::string& model_name, const csrc::DecodeOptions& opt,
                  std::vector<csrc::Detection>& out, int& frame_w, std::string& err) {
    out.clear();
    frame_w = 0;

    // 锁罩住"换模型 + 推理"整段：TPU 是单实例，YoloDetector 非线程安全；
    // 半路换模型或两个请求并发进来都会出问题。
    std::lock_guard<std::mutex> lk(ctx.detect_mu);

    if (!ctx.detector) ctx.detector.reset(new csrc::YoloDetector());
    // 模型只有一个来源：model_path()（= $AKA_HOME/demo/models/<名字>.cvimodel）
    const std::string path = model_path(ctx, model_name);
    // 文件被换过（重新下载覆盖）也要重载 —— 一次 stat 的开销，换"覆盖即生效"。
    struct stat st {};
    const bool have = (stat(path.c_str(), &st) == 0);
    const bool changed = have && (st.st_mtime != ctx.detect_mtime ||
                                  (long long)st.st_size != ctx.detect_size);
    if (!ctx.detector->loaded() || ctx.detect_model != model_name || changed) {
        if (!ctx.detector->load(path, err)) {
            ctx.detect_model.clear();
            return false;
        }
        ctx.detect_model = model_name;
        ctx.detect_mtime = have ? st.st_mtime : 0;
        ctx.detect_size = have ? (long long)st.st_size : 0;
    }

    if (!ensure_camera(ctx)) {
        err = "camera not available";
        return false;
    }
    // 取帧用**原生采集宽度**（camera.width），不是浏览器的 stream_width。
    // stream_width 是为了省浏览器带宽而降采样的（默认 320），拿它喂 640x480 的模型
    // 等于先把画面砍掉一半再放大回去（白丢分辨率）；更要命的是返回的框就落在那张
    // 320 宽帧的坐标系里，而 /api/camera/snapshot 给的是原生 640 宽帧 —— 两者差一倍，
    // 调用方把框画到快照上就会整体跑偏（实测框跑到画面左上角的背景上）。
    // 用 camera.width 还能和板载屏显示的 decode_max_w 同档，共用同一次解码。
    csrc::Camera::RgbFrame rgb;
    if (!ctx.camera.latest_rgb(ctx.config.camera.width, rgb) || rgb.data.empty()) {
        err = "no frame";
        return false;
    }
    frame_w = rgb.w;
    // 取帧（含解码，走 Camera::latest_rgb 的共享缓存）单独计一下：
    // 慢在"取帧"还是"推理"，决定了该优化哪条路。
    const auto t_det0 = std::chrono::steady_clock::now();
    const bool ok = ctx.detector->detect(rgb.data.data(), rgb.w, rgb.h, opt, out, err);
    const double det_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t_det0).count();
    CAM_DEBUG("[detect/frame] 取帧(含解码) 已完成，detect() 耗时 %.1fms", det_ms);
    return ok;
}

csrc::DecodeOptions decode_options(double conf, double iou) {
    csrc::DecodeOptions opt;   // 默认 conf=0.25 / iou=0.45
    // 0 = 调用方没给；给了就夹到 0.01~0.99（0 会"什么都留"，1 会"什么都不要"，
    // 两个极端都不是能用阈值，夹住比报错省事）
    if (conf > 0) opt.conf = (float)std::min(0.99, std::max(0.01, conf));
    if (iou > 0) opt.iou = (float)std::min(0.99, std::max(0.01, iou));
    return opt;
}

csrc::Json detect_once(AppContext& ctx, const std::string& model_name,
                       const csrc::DecodeOptions& opt) {
    csrc::Json j;
    std::vector<csrc::Detection> dets;
    int frame_w = 0;
    std::string err;
    if (!detect_boxes(ctx, model_name, opt, dets, frame_w, err)) {
        j["ok"] = false;
        j["error"] = err;
        return j;
    }

    csrc::Json boxes(csrc::Json::Type::Array);
    for (const auto& d : dets) {
        csrc::Json b;
        b["x1"] = csrc::Json((double)d.box.x1);
        b["y1"] = csrc::Json((double)d.box.y1);
        b["x2"] = csrc::Json((double)d.box.x2);
        b["y2"] = csrc::Json((double)d.box.y2);
        boxes.push_back(b);
    }
    j["ok"] = true;
    j["count"] = csrc::Json((int64_t)dets.size());
    j["boxes"] = boxes;
    return j;
}


// 启动屏显示（幂等）。摄像头未开时按需打开（屏要画面就得有摄像头）——
// 摄像头打开后 ensure_camera 内部也会自动启动显示，两条路都通。

}  // namespace capp
