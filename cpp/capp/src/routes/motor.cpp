// Motor 控制 + 底盘指令
//
// 入口：register_motor_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 对应 app/routes/motor.py（外加 /api/control）。

#include "routes_internal.hpp"

#include <cstdlib>

namespace capp {
namespace routes {

// ── Motor 控制 + 底盘指令 ──

void register_motor_routes(Router& router, AppContext& ctx) {

    // ── /api/control ──
    router.add("GET", "/api/control", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        std::string action = req.query_param("action");
        double speed = atof(req.query_param("speed", "50").c_str());
        if (speed < 1) speed = 1;
        if (speed > 100) speed = 100;
        int motor_speed = (int)(speed + 0.5);

        std::string dist_s = req.query_param("distance");
        std::string angle_s = req.query_param("angle");
        bool has_dist = !dist_s.empty();
        bool has_angle = !angle_s.empty();
        double distance = has_dist ? atof(dist_s.c_str()) : 0;
        double angle = has_angle ? atof(angle_s.c_str()) : 0;

        if (has_angle && action != "left" && action != "right") {
            Json err;
            err["status"] = "error";
            err["message"] = "angle 仅对 left/right 动作有效";
            resp.set_json(err, 400);
            return;
        }
        if (has_dist && action != "up" && action != "down") {
            Json err;
            err["status"] = "error";
            err["message"] = "distance 仅对 up/down 动作有效";
            resp.set_json(err, 400);
            return;
        }

        Json result;
        if ((action == "up" || action == "down") && has_dist) {
            std::string dir = action == "up" ? "forward" : "backward";
            result = move_distance(ctx, dir, distance, motor_speed);
        } else if ((action == "left" || action == "right") && has_angle) {
            result = move_distance(ctx, action, angle, motor_speed);
        } else {
            double ms = atof(req.query_param("time", "0").c_str());
            // 带时长(time>0)：同步执行完(自动停车)才回 ACK
            result = execute_action(ctx, action, motor_speed, ms, ms > 0);
            if (result.gets("status") == "error") {
                // 错误也用同一套形状（{completed,error}），别让调用方为失败另写一套解析
                Json e;
                e["completed"] = false;
                e["error"] = result.gets("message");
                resp.set_json(e, 400);
                return;
            }
            // grab/release 是异步的（夹爪序列约 3.5s）—— 等它做完，否则"completed"是假的
            if (action == "grab" || action == "release") wait_arm_done(ctx);
        }
        // **精简返回**：客户端（含嵌入式调用方）只需要"做完了没有"这一个标志，
        // 省得为了一堆字段写解析。细节要看就去 /api/motor/status 或日志。
        // completed=false 时附一句 error（否则出问题只能靠猜）。
        Json out;
        // 有的分支不产出 completed 字段（比如"命令已生效"这类）——那时以 status 为准，
        // 别把 "success" 当成失败原因（曾经因此回过 {"completed":false,"error":"success"}）
        out["completed"] = (result.get("completed") != nullptr)
                               ? result.getb("completed", false)
                               : result.gets("status") == "success";
        if (!out.getb("completed")) {
            const std::string why = result.gets("message");
            out["error"] = why.empty() ? result.gets("status") : why;
        }
        resp.set_json(out);
    });

    // ── /api/motor ──
    router.add("GET", "/api/motor/status", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        csrc::RobotStatus s = ctx.collector.get_status();
        Json j;
        j["left_speed"] = s.left_speed;
        j["right_speed"] = s.right_speed;
        j["left_target"] = csrc::Json((int64_t)s.left_target);
        j["right_target"] = csrc::Json((int64_t)s.right_target);
        j["gripper_status"] = s.gripper_status;
        j["gripper_target"] = csrc::Json((int64_t)s.gripper_target);
        j["motor"] = motor_status_json(ctx);
        resp.set_json(j);
    });

    router.add("GET", "/api/motor/direct", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        int left = (int)atof(req.query_param("left", "0").c_str());
        int right = (int)atof(req.query_param("right", "0").c_str());
        double duration = atof(req.query_param("duration", "0").c_str());
        try {
            // 带时长(duration>0)：同步执行完(自动停车)才回 ACK
            Json result = run_motor(ctx, left, right, duration, duration > 0);
            csrc::RobotStatus s = ctx.collector.get_status();
            result["left_speed"] = s.left_speed;
            result["right_speed"] = s.right_speed;
            resp.set_json(result);
        } catch (...) {
            Json err;
            err["error"] = "motor control failed";
            resp.set_json(err, 500);
        }
    });

    router.add("GET", "/api/motor/raw_command", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(send_raw_command(ctx, req.query_param("cmd")));
    });
}
}  // namespace routes
}  // namespace capp
