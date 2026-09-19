// 摄像头
//
// 入口：register_camera_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 对应 app/routes/camera.py。

#include "routes_internal.hpp"

#include "csrc/base64.hpp"
#include "csrc/log.hpp"
#include <cstdlib>
#include <thread>

namespace capp {
namespace routes {

// ── 摄像头 ──

void register_camera_routes(Router& router, AppContext& ctx) {


    // ── /api/camera ──
    // 注：屏显示跟随摄像头开关，但对前端透明 ——
    //     open 后台自动出图、close 后台自动清屏，接口不暴露屏状态。
    router.add("GET", "/api/camera/status", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json j;
        j["camera_on"] = ctx.camera_on && ctx.camera.is_available();
        resp.set_json(j);
    });

    router.add("POST", "/api/camera/open", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        bool ok = ensure_camera(ctx);   // 摄像头打开 → 屏随之开始显示（对前端透明）
        Json j;
        j["camera_on"] = ok && ctx.camera.is_available();
        resp.set_json(j, ok ? 200 : 500);
    });

    router.add("POST", "/api/camera/close", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        close_camera(ctx);              // 摄像头关闭 → 屏清屏熄灭（对前端透明）
        Json j;
        j["camera_on"] = false;
        resp.set_json(j);
    });

    router.add("GET", "/api/camera/snapshot", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        ensure_camera(ctx);
        std::vector<uint8_t> jpeg;
        if (!current_jpeg(ctx, 70, jpeg)) {
            resp.set_error("camera not available", 500);
            return;
        }
        int w = 0, h = 0;
        csrc::Camera::jpeg_get_size(jpeg.data(), jpeg.size(), w, h);
        Json j;
        j["image"] = csrc::base64_encode(jpeg.data(), jpeg.size());
        j["width"] = csrc::Json((int64_t)w);
        j["height"] = csrc::Json((int64_t)h);
        j["format"] = "jpeg";
        j["m"] = ctx.config.calib_m;
        j["c"] = ctx.config.calib_c;
        resp.set_json(j);
    });

    // MJPEG 流（Python /api/camera/stream 契约，支持 ?fps=N 覆盖，默认 15fps）
    router.add("GET", "/api/camera/stream", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn& conn, AppContext&) {
        resp.stream = true;
        ensure_camera(ctx);
        int fps = 15;
        std::string fps_s = req.query_param("fps");
        if (!fps_s.empty()) { fps = atoi(fps_s.c_str()); if (fps < 1) fps = 1; if (fps > 30) fps = 30; }
        auto min_interval = std::chrono::milliseconds(1000 / fps);

        std::string head =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Pragma: no-cache\r\n"
            "Expires: 0\r\n"
            "Connection: close\r\n\r\n";
        conn.write_all(head);

        uint64_t last_ts = 0;
        auto last_send = std::chrono::steady_clock::now();
        // stream_scale 配置开关 + 尺寸>0 → 服务端缩放重编码后下发（省 WiFi 带宽）；
        // 关闭或尺寸无效 → 直通原帧。
        const bool downscale = ctx.config.camera.stream_scale &&
                               ctx.config.camera.stream_width > 0 &&
                               ctx.config.camera.stream_height > 0;
        // 编码输出缓冲跨帧复用，避免每帧 malloc（长时间运行更稳）
        std::vector<uint8_t> jpeg;
        // 有浏览器在看流 → 屏显示降帧（[display] fps_streaming，0=暂停）：
        // 单核 SoC 上"显示 + 取流"会 CPU 饱和，拖慢网页看摄像头的帧率/延迟。
        ctx.display.set_streaming(true);
        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_send >= min_interval) {
                bool ok = false;
                uint64_t ts = 0;
                auto t0 = std::chrono::steady_clock::now();
                if (downscale) {
                    // 缩放路径：解码走 Camera::latest_rgb **共享缓存** ——
                    // 屏显示线程通常已解过这一帧，这里直接命中，省掉整帧解码
                    // （640x360 MJPEG → 320 宽，C906 上约 10ms/帧）。
                    csrc::Camera::RgbFrame rgb;
                    if (ctx.camera.latest_rgb(ctx.config.camera.stream_width, rgb) &&
                        !rgb.data.empty() && rgb.ts_ms != last_ts) {
                        jpeg.clear();
                        ok = build_stream_jpeg_rgb(ctx, rgb, jpeg);
                        ts = rgb.ts_ms;
                    }
                } else {
                    // 直通路径：原帧直接下发，完全不碰解码
                    csrc::Camera::Frame f;
                    if (ctx.camera.read_latest(f) && !f.data.empty() && f.ts_ms != last_ts &&
                        csrc::Camera::is_jpeg(f.data.data(), f.data.size())) {
                        jpeg = std::move(f.data);
                        ok = true;
                        ts = f.ts_ms;
                    }
                }
                double enc_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - t0).count();
                // 诊断：单帧耗时 >120ms 即肉眼可见卡顿，记录一次(每帧, debug 级)
                if (enc_ms > 120.0) {
                    CAM_DEBUG("camera stream frame encode %.0fms (cpu busy? size=%zu)",
                              enc_ms, jpeg.size());
                }
                if (ok && !jpeg.empty()) {
                    // MJPEG 直通/重编码帧：头 + jpeg + 尾拼成一个 buffer 一次 write（减少系统调用）
                    std::string part = "--frame\r\nContent-Type: image/jpeg\r\n"
                                       "Content-Length: " + std::to_string(jpeg.size()) +
                                       "\r\n\r\n";
                    std::string out;
                    out.reserve(part.size() + jpeg.size() + 2);
                    out += part;
                    out.append((const char*)jpeg.data(), jpeg.size());
                    out += "\r\n";
                    if (!conn.write_all(out)) break;
                    last_ts = ts;
                    last_send = now;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ctx.display.set_streaming(false);   // 没人看流了 → 屏恢复 fps
        conn.close();
    });

    router.add("GET", "/api/camera/speed", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        csrc::RobotStatus s = ctx.collector.get_status();
        Json j;
        j["left_speed"] = s.left_speed;
        j["right_speed"] = s.right_speed;
        j["left_target"] = csrc::Json((int64_t)s.left_target);
        j["right_target"] = csrc::Json((int64_t)s.right_target);
        j["gripper_status"] = s.gripper_status;
        j["gripper_target"] = csrc::Json((int64_t)s.gripper_target);
        j["timestamp_ms"] = csrc::Json((int64_t)s.timestamp_ms);
        resp.set_json(j);
    });

    router.add("GET", "/api/camera/all_status", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        csrc::RobotStatus s = ctx.collector.get_status();
        Json j;
        j["timestamp"] = req.query_param("timestamp");
        j["left_speed"] = s.left_speed;
        j["right_speed"] = s.right_speed;
        j["left_target"] = csrc::Json((int64_t)s.left_target);
        j["right_target"] = csrc::Json((int64_t)s.right_target);
        j["gripper_status"] = s.gripper_status;
        j["gripper_target"] = csrc::Json((int64_t)s.gripper_target);
        j["timestamp_ms"] = csrc::Json((int64_t)s.timestamp_ms);
        j["motor"] = motor_status_json(ctx);

        std::vector<uint8_t> jpeg;
        if (current_jpeg(ctx, 25, jpeg) && !jpeg.empty()) {
            j["image"] = csrc::base64_encode(jpeg.data(), jpeg.size());
        } else {
            j["image"] = Json();
        }
        j["image_format"] = "jpeg";
        resp.set_json(j);
    });
}
}  // namespace routes
}  // namespace capp
