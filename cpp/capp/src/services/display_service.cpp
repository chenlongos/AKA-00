// 板载屏显示服务
//
// 由 capp/src/services.cpp 按域拆出来（对应 app/services/*.py 的分法）。
// Python 原版没有屏，这一层是 C++ 侧新增的。
// 声明都在 capp/context.hpp（那一份是按域分节的伞头文件，调用方只 include 它）。

#include "capp/context.hpp"

#include "csrc/log.hpp"

namespace capp {

// 在"摄像头已开"的前提下启动屏显示（不再回调 ensure_camera，避免递归）。
// 不是 static：camera_service.cpp 里"摄像头一开就跟着起屏"也要用它（声明在 context.hpp）。
bool start_display_locked_on_camera(AppContext& ctx) {
    csrc::DisplayConfig dc = ctx.config.display;
    if (dc.decode_max_w <= 0) dc.decode_max_w = ctx.config.camera.stream_width;
    if (!ctx.display.start(dc)) {
        CAM_INFO("[display] screen disabled (no framebuffer)");
        return false;
    }
    return true;
}

// 启动屏显示（幂等）。摄像头未开时按需打开（屏要画面就得有摄像头）——
// 摄像头打开后 ensure_camera 内部也会自动启动显示，两条路都通。
bool ensure_display(AppContext& ctx) {
    if (ctx.display.running()) return true;
    if (!ctx.config.display.enabled) return false;
    // 显示依赖摄像头最新帧（复用 Camera 单例，不额外占用设备）
    if (!ensure_camera(ctx)) {
        CAM_WARN("[display] camera unavailable — screen off");
        return false;
    }
    return start_display_locked_on_camera(ctx);
}

void close_display(AppContext& ctx) { ctx.display.stop(); }

csrc::Json display_config(AppContext& ctx) {
    csrc::Json j;
    j["enabled"] = ctx.config.display.enabled;      // 配置/当前开关状态
    j["running"] = ctx.display.running();           // 显示线程是否真在跑
    j["available"] = ctx.display.available();       // 有没有 /dev/fb0
    j["scale"] = csrc::Json((int64_t)ctx.config.display.scale);
    j["fps"] = csrc::Json((int64_t)ctx.config.display.fps);
    if (ctx.display.running()) {
        const csrc::ScreenDisplay::Stats st = ctx.display.stats();
        j["region_w"] = csrc::Json((int64_t)st.out_w);
        j["region_h"] = csrc::Json((int64_t)st.out_h);
        j["frames"] = csrc::Json((int64_t)st.frames);
    }
    return j;
}

csrc::Json set_display_enabled(AppContext& ctx, bool enabled) {
    ctx.config.display.enabled = enabled;
    csrc::Json j;
    if (!enabled) {
        close_display(ctx);      // 立刻停屏（关摄像头路径也会调，幂等）
        CAM_INFO("[display] 运行时关闭屏显示");
    } else {
        // 打开：摄像头已开才会真的起屏（起不来不算错误，如实回报 running=false）
        ensure_display(ctx);
        CAM_INFO("[display] 运行时打开屏显示 (running=%d)", (int)ctx.display.running());
    }
    j["ok"] = true;
    j["enabled"] = ctx.config.display.enabled;
    j["running"] = ctx.display.running();
    return j;
}

}  // namespace capp
