// capp/routes.hpp — 路由注册声明

#pragma once

#include "capp/context.hpp"
#include "capp/http_server.hpp"

namespace capp {

/// 注册全部 HTTP 路由（control/motor/arm/camera/demo/ota/system/wifi/config/ws）
void register_routes(Router& router, AppContext& ctx);

}  // namespace capp
