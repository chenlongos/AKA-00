// capp/main.cpp — 独立 HTTP 服务入口（SG2002 直跑，无需 Python / 任何外部运行时）
//
// 架构：capp（HTTP+WS 服务层）→ csrc（硬件库：电机/夹爪/摄像头/状态采集）
// 部署：bin/aka-capp（riscv64 musl 静态），config.toml 走 [web] port / [motor] / [arm]

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>

#include "capp/context.hpp"
#include "capp/http_server.hpp"
#include "capp/routes.hpp"
#include "csrc/log.hpp"
#include "csrc/system_utils.hpp"

namespace {

volatile sig_atomic_t g_stop = 0;
capp::AppContext* g_ctx = nullptr;   // 信号处理器 → 优雅关闭标记
void on_signal(int) {
    g_stop = 1;
    if (g_ctx) g_ctx->shutdown = true;
}

}  // namespace

int main() {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    capp::AppContext ctx;
    g_ctx = &ctx;
    ctx.app_dir = getenv("AKA_HOME") ? getenv("AKA_HOME") : ".";
    ctx.static_dir = ctx.app_dir + "/static";

    CAM_INFO("╔══════════════════════════════════╗");
    CAM_INFO("║  AKA-00 capp (C++ standalone)   ║");
    CAM_INFO("╚══════════════════════════════════╝");

    // 服务层（硬件 + 状态采集）
    capp::init_services(ctx);

    // 板载屏显示（摄像头画面 → /dev/fb0）
    //
    // 屏跟随摄像头开关（固定行为，不是配置项）：
    //   开机时摄像头是关的 → 屏上显示熄屏待机图；
    //   前端打开摄像头（POST /api/camera/open）→ ensure_camera 自动启动显示，切实时画面；
    //   前端关闭摄像头（POST /api/camera/close）→ 回到待机图。
    // 显示与浏览器 /api/camera/stream 共享同一份解码结果（Camera::latest_rgb 缓存），
    // 因此开屏不会拖慢浏览器看摄像头的速度/效率。
    //
    // **不带屏版（AKA_WITH_SCREEN=0）整段编掉**：那块显示栈根本不存在（csrc 里全是桩），
    // 留着的话还会打一条"当前屏显示熄屏待机图 …/start_img.jpg"的假日志 ——
    // 不带屏包里连那张图都没打进去，排查时非常误导。
#if AKA_WITH_SCREEN
    if (ctx.config.display.enabled) {
        if (ctx.camera_on) {
            capp::ensure_display(ctx);
        } else {
            // 摄像头还没开 → 屏上显示「熄屏待机图」（原来是纯黑清屏）：
            // 打开摄像头会自动清屏切实时画面，关闭摄像头又回到这张图。
            std::string img = ctx.config.display.standby_image;
            if (!img.empty() && img[0] != '/') img = ctx.app_dir + "/" + img;
            if (!csrc::ScreenDisplay::show_standby_once(img, ctx.config.display)) {
                csrc::ScreenDisplay::clear_screen_once();
            }
            CAM_INFO("[display] 等摄像头打开后出图（当前屏显示熄屏待机图 %s）", img.c_str());
        }
    }
#endif  // AKA_WITH_SCREEN

    // 云端状态上报
    capp::start_status_reporter(ctx);

    // HTTP 服务
    capp::HttpServer server(ctx);
    server.router().set_static_dir(ctx.static_dir);
    server.router().set_index_file("index.html");
    capp::register_routes(server.router(), ctx);

    if (!server.listen(ctx.config.web.port)) {
        CAM_ERROR("failed to bind port %d", ctx.config.web.port);
        return 1;
    }

    // 上次连过的 WiFi 自动重连（后台线程）。
    //
    // 两个约束，改这段前先看一眼：
    //   1. **必须在 listen() 之后**。ensure_wpa_env 最坏等 5s、整个重放可能几十秒，
    //      挡在端口绑定前面的话，手机连上热点会打不开页面 —— 那是本功能最容易踩的回归。
    //   2. 线程 detach 且**不捕获 ctx**（只读 /etc/aka-wifi.json + 跑 wpa_cli），
    //      所以下面那段退出清理不需要 join 它。往线程里加 ctx 引用会引入悬垂。
    capp::start_wifi_autoconnect();

    // HTTPS：相对路径以 $AKA_HOME 为基准
    auto resolve_path = [&](const std::string& p) -> std::string {
        return (!p.empty() && p[0] == '/') ? p : ctx.app_dir + "/" + p;
    };
    if (ctx.config.web.https_port > 0) {
        std::string cert_path = resolve_path(ctx.config.web.https_cert);
        std::string key_path  = resolve_path(ctx.config.web.https_key);
        if (!server.listen_tls(ctx.config.web.https_port, cert_path, key_path)) {
            CAM_WARN("[app] TLS listener disabled (HTTP still serving on :%d)",
                     ctx.config.web.port);
        } else {
            CAM_INFO("[app] https://0.0.0.0:%d (wss: /ws/control)", ctx.config.web.https_port);
        }
    }

    CAM_INFO("[app] static dir = %s", ctx.static_dir.c_str());
    CAM_INFO("[app] http://0.0.0.0:%d (ws: /ws/control)", ctx.config.web.port);
    server.run();  // 阻塞直到 SIGTERM/SIGINT（on_signal → ctx.shutdown）

    // 退出清理
    capp::close_display(ctx);   // 先停显示线程（它要用摄像头最新帧）
    capp::close_camera(ctx);
    {
        std::lock_guard<std::mutex> lk(ctx.timer_mu);
        if (ctx.timer_thread) {
            ctx.timer_cancel = true;
            ctx.timer_thread->join();
            delete ctx.timer_thread;
            ctx.timer_thread = nullptr;
        }
    }
    ctx.collector.stop();
    ctx.motor_pair->close();
    CAM_INFO("[app] shutdown");
    return 0;
}
