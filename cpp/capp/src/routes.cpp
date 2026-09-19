// capp/routes.cpp — 路由注册的入口
//
// 对应 app/routes/*.py：**一个域一个文件**，都放在 src/routes/ 下，各自的注册函数由
// 这里按顺序调用（每个域只管把路由 add 进 router，互不依赖、也不互相覆盖）。
// 原来全在这一个文件里（1960 行、47 条路由混着 12 个域），按域拆开之后加一条路由
// 只用打开对应那个域的文件。
//
//     routes/motor.cpp    /api/control + /api/motor/*
//     routes/arm.cpp      /api/arm/*
//     routes/camera.cpp   /api/camera/*
//     routes/models.cpp   /api/detect + /api/models/* + /api/model/upload
//     routes/demo.cpp     /api/demo/*（卡片与动作脚本）
//     routes/display.cpp  /api/display/*
//     routes/ota.cpp      /api/ota/*
//     routes/system.cpp   /api/system/*
//     routes/wifi.cpp     /api/wifi/*
//     routes/config.cpp   /api/config/*
//     routes/ws.cpp       /ws/control
//
// 跨域共用的东西（demo 卡片读写、multipart 解析）在 routes/routes_internal.hpp 声明，
// 实现跟着各自"主要使用者"走：demo_card.cpp / multipart.cpp。

#include "capp/routes.hpp"

#include "routes/routes_internal.hpp"

namespace capp {

void register_routes(Router& router, AppContext& ctx) {
    routes::register_motor_routes(router, ctx);
    routes::register_arm_routes(router, ctx);
    routes::register_camera_routes(router, ctx);
    routes::register_models_routes(router, ctx);
    routes::register_demo_routes(router, ctx);
    routes::register_display_routes(router, ctx);
    routes::register_ota_routes(router, ctx);
    routes::register_system_routes(router, ctx);
    routes::register_wifi_routes(router, ctx);
    routes::register_config_routes(router, ctx);
    routes::register_ws_routes(router, ctx);
}

}  // namespace capp
