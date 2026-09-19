// 板载屏显示开关
//
// 入口：register_display_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// Python 原版没有屏，这一域是 C++ 侧新增的。

#include "routes_internal.hpp"


namespace capp {
namespace routes {

// ── 板载屏显示开关 ──

void register_display_routes(Router& router, AppContext& ctx) {



    // ── 屏显示开关 ──
    // 为什么要有：全屏写屏很吃那颗单核 CPU（实测 /api/detect 从 120ms 涨到 340ms），
    // 要在跑检测/追物时让出 CPU 就把它关掉；想看屏就再打开。
    // 只改运行时状态，不写 config.toml（重启后回到文件里的值）。
    router.add("GET", "/api/display/status", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(display_config(ctx));
    });

    router.add("POST", "/api/display/enabled", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        // 也接受 ?enabled=0/1（前端表单有时更顺手）
        const std::string q = req.query_param("enabled");
        bool enabled;
        if (!q.empty()) {
            enabled = (q == "1" || q == "true" || q == "yes");
        } else if (payload.get("enabled")) {
            enabled = payload.getb("enabled", true);
        } else {
            resp.set_error("enabled 必填（true/false）", 400);
            return;
        }
        resp.set_json(set_display_enabled(ctx, enabled));
    });
}
}  // namespace routes
}  // namespace capp
