// 速度配置
//
// 入口：register_config_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 对应 app/routes/config.py。

#include "routes_internal.hpp"

#include <fstream>
#include <sstream>

namespace capp {
namespace routes {

std::string speed_config_path(AppContext& ctx) {
    return ctx.app_dir + "/speed_config.json";
}

Json load_speed_config(AppContext& ctx) {
    Json out;
    out["forward_speed"] = Json((int64_t)50);
    out["turn_speed"] = Json((int64_t)50);
    std::ifstream f(speed_config_path(ctx));
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        Json data;
        if (Json::parse(ss.str(), data) && data.is_object()) {
            out["forward_speed"] = Json(data.geti("forward_speed", 50));
            out["turn_speed"] = Json(data.geti("turn_speed", 50));
        }
    }
    return out;
}

bool save_speed_config(AppContext& ctx, const Json& payload) {
    Json cfg;
    cfg["forward_speed"] = Json(payload.geti("forward_speed", 50));
    cfg["turn_speed"] = Json(payload.geti("turn_speed", 50));
    std::ofstream f(speed_config_path(ctx));
    if (!f) return false;
    f << cfg.dump(false);
    return true;
}

// ── 速度配置 ──

void register_config_routes(Router& router, AppContext& ctx) {


    // ── /api/config ──
    router.add("GET", "/api/config/speed", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(load_speed_config(ctx));
    });

    router.add("POST", "/api/config/speed", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body required", 400);
            return;
        }
        if (!save_speed_config(ctx, payload)) {
            resp.set_error("write speed_config.json failed", 500);
            return;
        }
        resp.set_json(load_speed_config(ctx));
    });
}
}  // namespace routes
}  // namespace capp
