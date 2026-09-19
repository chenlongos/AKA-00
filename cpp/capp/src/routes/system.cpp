// 系统信息
//
// 入口：register_system_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 对应 app/routes/system.py。

#include "routes_internal.hpp"


#include "csrc/system_utils.hpp"

namespace capp {
namespace routes {

// ── 系统信息 ──

void register_system_routes(Router& router, AppContext& ctx) {
    (void)ctx;   // 这一域的路由用不到 ctx —— 签名保持一致，调用方一视同仁


    // ── /api/system ──
    router.add("GET", "/api/system/info", [](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json j;
        j["ip"] = csrc::detect_local_ip();
        j["mac"] = csrc::mac_address("wlan0");
        resp.set_json(j);
    });

    router.add("GET", "/api/system/ip", [](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json j;
        j["ip"] = csrc::detect_local_ip();
        resp.set_json(j);
    });

    router.add("GET", "/api/system/heartbeat", [](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json j;
        j["status"] = "ok";
        j["service"] = "AKA-00";
        j["mac_address"] = csrc::mac_address("wlan0");
        j["cpu"] = csrc::Json((int64_t)csrc::cpu_usage());
        j["mem"] = csrc::Json((int64_t)csrc::mem_usage());
        j["disk"] = csrc::Json((int64_t)csrc::disk_usage());
        j["uptime"] = csrc::Json((int64_t)csrc::uptime_secs());
        resp.set_json(j);
    });
}
}  // namespace routes
}  // namespace capp
