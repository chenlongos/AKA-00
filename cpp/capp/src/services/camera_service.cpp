// 摄像头服务
//
// 由 capp/src/services.cpp 按域拆出来（对应 app/services/*.py 的分法）。
// 对应 app/services/camera_service.py。
// 声明都在 capp/context.hpp（那一份是按域分节的伞头文件，调用方只 include 它）。

#include "capp/context.hpp"

#include "csrc/camera.hpp"
#include "csrc/log.hpp"

namespace capp {

bool ensure_camera(AppContext& ctx) {
    if (ctx.camera_on) return true;
    // 固定曝光（可选，须在 open 前设置；对应 demo 的 DEMO_EXP_FIX=1）
    ctx.camera.set_fixed_exposure(ctx.config.camera.exp_fix);
    bool ok = ctx.camera.open(ctx.config.camera.width, ctx.config.camera.height,
                              ctx.config.camera.fps);
    ctx.camera_on = ok;
    // 屏显示跟随摄像头（固定行为）：摄像头一开，屏就出图。
    // **不带屏版整段编掉**：这台机器没有屏这层，走到这里只会打一句"屏开始显示"的假日志，
    // 然后一路掉进 csrc 的桩里 —— 对用户来说"有没有屏"应该是透明的，日志里不该出现屏。
#if AKA_WITH_SCREEN
    if (ok && ctx.config.display.enabled) {
        if (!ctx.display.running()) {
            CAM_INFO("[display] 摄像头已开 → 屏开始显示");
            start_display_locked_on_camera(ctx);
        }
    }
#endif
    return ok;
}

void close_camera(AppContext& ctx) {
    // 先停显示（stop() 会清屏熄屏），再关摄像头
    close_display(ctx);
    ctx.camera.close();
    ctx.camera_on = false;
}

bool current_jpeg(AppContext& ctx, int quality, std::vector<uint8_t>& out) {
    csrc::Camera::Frame f;
    if (!ctx.camera.read_latest(f) || f.data.empty()) return false;
    if (csrc::Camera::is_jpeg(f.data.data(), f.data.size())) {
        out = std::move(f.data);
        return true;
    }
    // YUYV → RGB → JPEG
    if (f.format == 0 || f.w <= 0 || f.h <= 0) return false;
    std::vector<uint8_t> rgb((size_t)f.w * f.h * 3);
    csrc::Camera::yuyv_to_rgb(f.data.data(), f.w, f.h, rgb.data());
    return csrc::Camera::rgb_to_jpeg(rgb.data(), f.w, f.h, quality, out);
}

// 解码 → 等比缩放（黑边补齐到 stream_* 尺寸）→ 重编码 JPEG。
// 返回 false 表示该帧无法转出 JPEG（坏帧/未知格式），调用方应跳过而不是断开。
//
// 关键：解码走 Camera::latest_rgb（**共享缓存**）——屏幕显示线程与浏览器流共用
// 同一帧的解码结果，同一帧只解码一次。所以"开屏"不会拖慢浏览器：浏览器反而省掉
// 了自己那次整帧解码（640x360 MJPEG → 320 宽，C906 上约 10ms/帧）。

// 解码 → 等比缩放（黑边补齐到 stream_* 尺寸）→ 重编码 JPEG。
// 返回 false 表示该帧无法转出 JPEG（坏帧/未知格式），调用方应跳过而不是断开。
//
// 关键：解码走 Camera::latest_rgb（**共享缓存**）——屏幕显示线程与浏览器流共用
// 同一帧的解码结果，同一帧只解码一次。所以"开屏"不会拖慢浏览器：浏览器反而省掉
// 了自己那次整帧解码（640x360 MJPEG → 320 宽，C906 上约 10ms/帧）。
bool build_stream_jpeg(AppContext& ctx, const csrc::Camera::Frame& f,
                       std::vector<uint8_t>& out) {
    if (f.data.empty()) return false;
    csrc::Camera::RgbFrame rgb;
    if (!ctx.camera.latest_rgb(ctx.config.camera.stream_width, rgb) || rgb.data.empty())
        return false;
    return build_stream_jpeg_rgb(ctx, rgb, out);
}

// 由已解码的 RGB 帧出流（缓存命中路径）：缩放补齐 + 编码，不再重复解码。

// 由已解码的 RGB 帧出流（缓存命中路径）：缩放补齐 + 编码，不再重复解码。
bool build_stream_jpeg_rgb(AppContext& ctx, const csrc::Camera::RgbFrame& rgb,
                           std::vector<uint8_t>& out) {
    const int ow = ctx.config.camera.stream_width;
    const int oh = ctx.config.camera.stream_height;
    const int q = ctx.config.camera.stream_quality;
    if (ow <= 0 || oh <= 0 || q <= 0 || rgb.data.empty()) return false;
    const int w = rgb.w, h = rgb.h;
    if (w <= 0 || h <= 0) return false;

    if (w == ow && h == oh) {
        return csrc::Camera::rgb_to_jpeg(rgb.data.data(), w, h, q, out);
    }
    std::vector<uint8_t> box((size_t)ow * oh * 3);
    if (!csrc::Camera::letterbox_rgb(rgb.data.data(), w, h, box.data(), ow, oh))
        return false;
    return csrc::Camera::rgb_to_jpeg(box.data(), ow, oh, q, out);
}



/// 校验临时文件（CviModel 魔数 + 大小上限）后原子换入最终路径；失败时删掉临时文件并填 err。

}  // namespace capp
