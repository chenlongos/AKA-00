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

    CAM_INFO("[app] static dir = %s", ctx.static_dir.c_str());
    CAM_INFO("[app] http://0.0.0.0:%d (ws: /ws/control)", ctx.config.web.port);
    server.run();  // 阻塞直到 SIGTERM/SIGINT（on_signal → ctx.shutdown）

    // 退出清理
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
