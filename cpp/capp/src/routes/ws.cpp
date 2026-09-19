// WebSocket 控制通道升级
//
// 入口：register_ws_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 实时控制通道，见 capp/websocket.cpp。

#include "routes_internal.hpp"

#include "capp/websocket.hpp"

namespace capp {
namespace routes {

// ── WebSocket 控制通道升级 ──

void register_ws_routes(Router& router, AppContext& ctx) {


    // ── /ws/control ──
    router.add("GET", "/ws/control", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn& conn, AppContext&) {
        std::string upgrade = req.header("upgrade");
        if (upgrade.find("websocket") == std::string::npos) {
            resp.set_error("websocket upgrade required", 400);
            return;
        }
        resp.stream = true;  // 接管连接
        if (!ws_handshake(req, conn)) {
            conn.close();
            return;
        }
        ws_control_loop(ctx, conn);
    });
}
}  // namespace routes
}  // namespace capp
