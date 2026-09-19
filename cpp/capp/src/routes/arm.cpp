// 机械臂角度
//
// 入口：register_arm_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 对应 app/routes/arm.py。

#include "routes_internal.hpp"

#include <fstream>
#include <sstream>

namespace capp {
namespace routes {

// ── 机械臂角度 ──

void register_arm_routes(Router& router, AppContext& ctx) {


    // ── /api/arm ──
    router.add("GET", "/api/arm/angles", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json j;
        j["driver"] = ctx.config.arm.backend;
        j["angles"] = csrc::load_arm_angles(ctx.config.arm.backend);
        resp.set_json(j);
    });

    router.add("POST", "/api/arm/angles", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        std::string driver = ctx.config.arm.backend;
        if (payload.has("driver")) driver = payload.gets("driver");
        if (driver != ctx.config.arm.backend) {
            resp.set_error("driver mismatch: expected " + ctx.config.arm.backend + ", got " + driver, 400);
            return;
        }
        const Json* angles = payload.get("angles");
        if (!angles || !angles->is_object()) angles = &payload;

        Json normalized = csrc::save_arm_angles(driver, *angles);
        Json upd = update_arm_angles(ctx, driver, normalized);
        if (upd.has("error")) {
            resp.set_error(upd.gets("error"), 400);
            return;
        }
        Json j;
        j["status"] = "success";
        j["driver"] = driver;
        j["angles"] = normalized;
        resp.set_json(j);
    });

    // /api/arm/angles/default
    router.add("GET", "/api/arm/angles/default", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        std::string path = csrc::arm_angles_path();
        size_t slash = path.find_last_of('/');
        std::string def_file = (slash == std::string::npos ? "" : path.substr(0, slash + 1)) + "arm_angles_default.json";
        std::ifstream f(def_file);
        if (!f) {
            resp.set_error("default config not found", 404);
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        Json data;
        if (!Json::parse(ss.str(), data)) {
            resp.set_error("default config parse failed", 404);
            return;
        }
        Json j;
        j["driver"] = ctx.config.arm.backend;
        j["angles"] = data;
        resp.set_json(j);
    });

    router.add("POST", "/api/arm/angles/default", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body required", 400);
            return;
        }
        std::string driver = payload.gets("driver", ctx.config.arm.backend);
        if (driver != ctx.config.arm.backend) {
            resp.set_error("driver mismatch", 400);
            return;
        }
        const Json* angles = payload.get("angles");
        if (!angles || !angles->is_object()) angles = &payload;
        std::string path = csrc::arm_angles_path();
        size_t slash = path.find_last_of('/');
        std::string def_file = (slash == std::string::npos ? "" : path.substr(0, slash + 1)) + "arm_angles_default.json";
        std::ofstream f(def_file);
        if (!f) {
            resp.set_error("cannot write default config", 500);
            return;
        }
        f << angles->dump(true, 0);
        Json j;
        j["status"] = "success";
        j["driver"] = driver;
        j["angles"] = *angles;
        resp.set_json(j);
    });

    // /api/arm/angles/preview
    router.add("POST", "/api/arm/angles/preview", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        std::string driver = payload.gets("driver", ctx.config.arm.backend);
        if (driver != ctx.config.arm.backend) {
            resp.set_error("driver mismatch: expected " + ctx.config.arm.backend + ", got " + driver, 400);
            return;
        }
        std::string key = payload.gets("key");
        if (key.empty()) {
            resp.set_error("key is required", 400);
            return;
        }
        if (!payload.has("value")) {
            resp.set_error("value is required", 400);
            return;
        }
        const Json* angles = payload.get("angles");
        if (!angles || !angles->is_object()) {
            resp.set_error("angles is required", 400);
            return;
        }
        int value = (int)payload.geti("value", 0);

        Json normalized = csrc::save_arm_angles(driver, *angles);
        Json upd = update_arm_angles(ctx, driver, normalized);
        if (upd.has("error")) {
            resp.set_error(upd.gets("error"), 400);
            return;
        }
        Json prv = preview_arm_angle(ctx, driver, key, value);
        if (prv.has("error")) {
            resp.set_error(prv.gets("error"), 400);
            return;
        }
        Json j;
        j["status"] = "success";
        j["driver"] = driver;
        j["key"] = key;
        j["value"] = csrc::Json((int64_t)value);
        j["angles"] = normalized;
        resp.set_json(j);
    });
}
}  // namespace routes
}  // namespace capp
