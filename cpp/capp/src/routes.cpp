// capp/routes.cpp — 全部 HTTP 路由处理器
//
// 对应 app/routes/*.py（API 契约与 frontend/src/api.ts 完全对齐）
// 包含: control / motor / arm / camera / demo / ota / system / wifi / config / frontend

#include "capp/http_server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "capp/context.hpp"
#include "capp/websocket.hpp"
#include "csrc/angle_config.hpp"
#include "csrc/base64.hpp"
#include "csrc/http_client.hpp"
#include "csrc/log.hpp"
#include "csrc/system_utils.hpp"

namespace capp {

namespace {

using Json = csrc::Json;
using HttpResult = csrc::HttpResult;

// ═══════════════════════ 小工具 ═══════════════════════

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

// VERSION 文件: "v1.2.3@1722169200" 或 "v1.2.3 1722169200"
void read_version_file(AppContext& ctx, std::string& ver, int64_t& ts) {
    ver = "unknown";
    ts = 0;
    std::ifstream f(ctx.app_dir + "/VERSION");
    if (!f) return;
    std::string raw;
    std::getline(f, raw);
    if (raw.empty()) return;
    char sep = raw.find('@') != std::string::npos ? '@' : ' ';
    size_t pos = raw.rfind(sep);
    if (pos != std::string::npos) {
        ver = raw.substr(0, pos);
        ts = (int64_t)atoll(raw.substr(pos + 1).c_str());
    } else {
        ver = raw;
    }
}

// ── OTA: semver 解析 ──
// "v1.2.3" → (1,2,3,0)；"v1.2.3-4-gabc" → (1,2,3,4)；解析失败返回空
std::vector<int> parse_semver(const std::string& v) {
    std::vector<int> out;
    std::string s = v;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '-') { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    parts.push_back(cur);
    if (parts.empty()) return out;

    std::vector<std::string> nums;
    cur.clear();
    for (char c : parts[0]) {
        if (c == '.') { nums.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    nums.push_back(cur);
    try {
        int major = std::stoi(nums[0]);
        int minor = nums.size() > 1 ? std::stoi(nums[1]) : 0;
        int patch = nums.size() > 2 ? std::stoi(nums[2]) : 0;
        int commits = 0;
        if (parts.size() > 1) {
            try { commits = std::stoi(parts[1]); } catch (...) { commits = 0; }
        }
        out = {major, minor, patch, commits};
    } catch (...) {
        return {};
    }
    return out;
}

// ISO8601 → unix 秒（"2024-05-01T10:00:00.000Z" 等）
int64_t parse_iso_time(const std::string& t) {
    struct tm tm = {};
    int year, mon, day, hh, mm;
    double ss = 0;
    if (sscanf(t.c_str(), "%d-%d-%dT%d:%d:%lf", &year, &mon, &day, &hh, &mm, &ss) < 5) {
        if (sscanf(t.c_str(), "%d-%d-%d %d:%d:%lf", &year, &mon, &day, &hh, &mm, &ss) < 5) {
            return 0;
        }
    }
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = (int)ss;
    tm.tm_isdst = -1;
    time_t ts = timegm(&tm);
    return ts < 0 ? 0 : (int64_t)ts;
}

// 远端版本信息（OTA /check /upgrade 用）
Json fetch_release_info(AppContext& ctx) {
    Json empty;
    if (ctx.config.ota.check_url.empty()) return empty;
    HttpResult r = csrc::http_get(ctx.config.ota.check_url, 5);
    if (!r.ok) {
        CAM_WARN("[ota] check failed: %s", r.error.c_str());
        return empty;
    }
    Json data;
    if (!Json::parse(r.body, data)) return empty;
    const Json* inner = data.get("data");
    if (!inner || !inner->is_object()) inner = &data;
    if (!inner->is_object()) return empty;

    Json info;
    std::string url = inner->gets("imageUrl");
    if (url.empty()) url = inner->gets("url");
    info["url"] = url;
    info["version_number"] = inner->gets("versionNumber");
    info["hardware_desc"] = inner->gets("hardwareDesc");
    info["software_desc"] = inner->gets("softwareDesc");
    info["version"] = Json((int64_t)parse_iso_time(inner->gets("updatedAt")));
    return info;
}

// OTA 重启脚本（对应 Python _write_restart_script，杀掉旧 capp 再跑 update）
void write_restart_script(const std::string& firmware_path) {
    const char* server_name = getenv("AKA_SERVER_NAME");
    std::string name = server_name ? server_name : "aka-capp";

    std::string update_path = "/tmp/aka-ota-update";
    std::string mv = "mv -f \"" + firmware_path + "\" " + update_path;
    system(mv.c_str());
    chmod(update_path.c_str(), 0755);

    std::ofstream f("/tmp/aka-ota-install.sh");
    f << "#!/bin/sh\n"
         "set -e\n"
         "LOCK_FILE=\"/tmp/aka-ota-lock\"\n"
         "touch \"$LOCK_FILE\"\n"
         "sleep 3\n"
         "killall " << name << " 2>/dev/null || true\n"
         "sleep 2\n"
         "killall -9 " << name << " 2>/dev/null || true\n"
         "exec " << update_path << " --update\n";
    f.close();
    chmod("/tmp/aka-ota-install.sh", 0755);
    system("/bin/sh /tmp/aka-ota-install.sh >/dev/null 2>&1 &");
}

// multipart/form-data 文件提取（upload_model / OTA update 用）
// 返回文件内容；filename 由 Content-Disposition 提取
bool extract_multipart_file(const std::string& body, const std::string& content_type,
                            std::string& filename, std::string& content) {
    size_t bpos = content_type.find("boundary=");
    if (bpos == std::string::npos) return false;
    std::string boundary = content_type.substr(bpos + 9);
    // 去引号
    if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"') {
        boundary = boundary.substr(1, boundary.size() - 2);
    }
    std::string delim = "--" + boundary;
    size_t part_start = body.find(delim);
    if (part_start == std::string::npos) return false;
    part_start += delim.size();
    // 跳过第一段头
    size_t hdr_end = body.find("\r\n\r\n", part_start);
    if (hdr_end == std::string::npos) return false;
    std::string part_headers = body.substr(part_start, hdr_end - part_start);

    // 提取 filename
    size_t fn = part_headers.find("filename=\"");
    if (fn != std::string::npos) {
        fn += 10;
        size_t fn_end = part_headers.find('"', fn);
        if (fn_end != std::string::npos) filename = part_headers.substr(fn, fn_end - fn);
    }
    // 内容到下一个 --boundary
    size_t data_start = hdr_end + 4;
    size_t data_end = body.find("\r\n--" + boundary, data_start);
    if (data_end == std::string::npos) data_end = body.size();
    content = body.substr(data_start, data_end - data_start);
    return true;
}

// ── demo: 扫描含 init.sh 的子目录 ──
// 板上"能跑的 demo" = models/ 里有哪个模型（demo 名就是模型名，跑同一条 chase 流程）。
// 原来这里扫的是 demo/ 目录（预编译二进制 + init.sh），那套已被 Lua 脚本取代。
struct DemoInfo {
    std::string name;
    std::string path;
};

std::vector<DemoInfo> list_demos(AppContext& ctx) {
    std::vector<DemoInfo> out;
    const std::string dir = ctx.app_dir + "/models";
    const std::string suffix = ".cvimodel";
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() <= suffix.size() ||
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        out.push_back({n.substr(0, n.size() - suffix.size()), dir + "/" + n});
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const DemoInfo& x, const DemoInfo& y) { return x.name < y.name; });
    return out;
}

// wpa_supplicant 自举（移植自 app/routes/wifi.py 的 ensure_wpa_env）
// 若 wlan1 的控制接口未就绪，则拉起网卡并后台启动 wpa_supplicant。
// 与 Python 版的区别：不执行 killall，避免误杀 wlan0 上服务当前连接的 wpa_supplicant。
bool ensure_wpa_env() {
    const std::string ctrl  = "/var/run/wpa_supplicant";
    const std::string iface = "wlan1";
    const std::string sock  = ctrl + "/" + iface;

    struct stat st{};
    if (stat(sock.c_str(), &st) == 0) return true;        // 已就绪
    if (stat(ctrl.c_str(), &st) != 0) mkdir(ctrl.c_str(), 0700);

    csrc::exec_output("ip link set " + iface + " down 2>/dev/null");
    csrc::exec_output("ip link set " + iface + " up 2>/dev/null");
    usleep(500000);
    csrc::exec_output("wpa_supplicant -D nl80211 -i " + iface + " -C " + ctrl +
                      " -B >/dev/null 2>&1");
    for (int i = 0; i < 10; i++) {                        // 最多等 5s
        if (stat(sock.c_str(), &st) == 0) return true;
        usleep(500000);
    }
    return false;
}

}  // namespace

// ═══════════════════════ 路由注册 ═══════════════════════

void register_routes(Router& router, AppContext& ctx) {
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
                resp.set_json(result, 400);
                return;
            }
        }
        resp.set_json(result);
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

    // 单帧推理：取当前帧跑一次模型，只回框的四个角（原图像素坐标）。
    // 模型必填（裸名字 → $AKA_HOME/models/<名字>.cvimodel），先不做阈值等 query 覆盖。
    router.add("GET", "/api/detect", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const std::string model = req.query_param("model");
        if (model.empty()) {
            Json j;
            j["ok"] = false;
            j["error"] = "缺少 model 参数（例：/api/detect?model=tennis）";
            resp.set_json(j, 400);
            return;
        }
        if (!valid_model_name(model)) {
            Json j;
            j["ok"] = false;
            j["error"] = "model 名字非法（只允许字母数字与 _ - .）：" + model;
            resp.set_json(j, 400);
            return;
        }
        const Json j = detect_once(ctx, model);
        resp.set_json(j, j.getb("ok") ? 200 : 500);
    });

    // 模型上传：**平台把模型文件直接推给小车**（小车在内网，未必能反过来访问平台）。
    // 名字走 query（?name=tennis），文件放请求体：
    //   raw：     curl --data-binary @tennis.cvimodel "http://<ip>/api/models/upload?name=tennis"
    //   multipart：curl -F "file=@tennis.cvimodel"  "http://<ip>/api/models/upload?name=tennis"
    // 同步返回（3.5MB 的体很小，写完即回），同名覆盖、覆盖即生效。
    router.add("POST", "/api/models/upload", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const std::string name = req.query_param("name");
        if (name.empty()) {
            resp.set_error("name 参数必填（例：?name=tennis）", 400);
            return;
        }
        if (!valid_model_name(name)) {
            resp.set_error("name 非法（只允许字母数字与 _ - .）：" + name, 400);
            return;
        }
        std::string content = req.body;
        const std::string ct = req.header("content-type");
        if (ct.find("multipart/form-data") != std::string::npos) {
            std::string filename;   // 名字以 ?name= 为准，这里只取文件内容
            if (!extract_multipart_file(req.body, ct, filename, content)) {
                resp.set_error("multipart 解析失败（缺 file 字段？）", 400);
                return;
            }
        }
        // 超过服务器上限的体不会被读进来（req.body 是空的），单独给个明确的原因，
        // 否则调用方只会看到含糊的"请求体为空"。
        const std::string cl_hdr = req.header("content-length");
        if (!cl_hdr.empty() && atoll(cl_hdr.c_str()) > kMaxRequestBody) {
            resp.set_error("文件过大：" + cl_hdr + " 字节，上限 " +
                               std::to_string(kMaxRequestBody / (1024 * 1024)) + "MB",
                           413);
            return;
        }
        if (content.empty()) {
            resp.set_error("请求体为空（把模型文件放进 body）", 400);
            return;
        }
        const Json r = save_model_upload(ctx, name, content);
        resp.set_json(r, r.getb("ok") ? 200 : 400);
    });

    // ── 流程脚本（scripts/*.lua）──
    // 把"看→对准→靠近→抓"这类要反复调参的流程写成脚本，改一行存盘重跑，不用重编部署。
    // 安全兜底（限速/总超时/被人的指令取代/底盘掉线/内存与卡死）全在宿主里，脚本绕不过去。
    router.add("POST", "/api/script/run", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        const std::string name = payload.gets("script");
        if (name.empty()) {
            resp.set_error("script 必填（例：chase）", 400);
            return;
        }
        const Json* params = payload.get("params");
        const int max_seconds = (int)payload.geti("max_seconds", 30);
        const Json r = script_run(ctx, name, params ? *params : Json(), max_seconds);
        resp.set_json(r, r.getb("ok") ? 200 : 400);
    });

    router.add("GET", "/api/script/status", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(script_status(ctx));
    });

    router.add("POST", "/api/script/stop", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(script_stop(ctx));
    });

    // ── /api/demo ──（**薄封装**：demo 现在就是"拿某个模型跑一遍 Lua 抓取流程"）
    // 路径与字段保持不变，前端 DemoPage 一行都不用改；行为则从预编译二进制变成了可改的脚本：
    // 调追物就改 scripts/chase.lua，改完 scp 上去即可，不用重编不用重启。
    router.add("GET", "/api/demo/list", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json demos;
        for (auto& d : list_demos(ctx)) {
            Json item;
            item["name"] = d.name;
            item["path"] = d.path;
            item["kind"] = "model";     // 原来是 binary（预编译 demo），现在是"脚本 + 模型"
            item["script"] = "chase";
            demos.push_back(item);
        }
        Json j;
        j["demos"] = demos;
        resp.set_json(j);
    });

    router.add("GET", "/api/demo/name", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json j;
        j["name"] = script_status(ctx).gets("script");
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/init", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        const std::string name = payload.is_object() ? payload.gets("name") : "";
        if (name.empty()) {
            resp.set_error("name is required", 400);
            return;
        }
        // 跑同一条 chase 流程，模型就是 demo 名。target_size/speed 这里给默认值，
        // 细调在 scripts/chase.lua 的判据常量里。
        Json params;
        params["model"] = name;
        params["target_size"] = Json((int64_t)payload.geti("target_size", 300));
        params["speed"] = Json((int64_t)payload.geti("speed", 25));
        const Json r = script_run(ctx, "chase", params, (int)payload.geti("max_seconds", 60));

        if (!r.getb("ok")) {
            const Json st = script_status(ctx);
            Json j;
            j["status"] = "already_running";
            j["pid"] = Json((int64_t)getpid());
            j["name"] = st.gets("script");
            j["error"] = r.gets("error");
            resp.set_json(j, 409);
            return;
        }
        Json j;
        j["status"] = "started";
        j["name"] = name;
        j["script"] = "chase";
        j["pid"] = Json((int64_t)getpid());   // 兼容字段：跑 demo 的进程就是 capp 自己
        j["pgid"] = Json((int64_t)getpid());
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/stop", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json r = script_stop(ctx);
        Json j;
        j["status"] = r.gets("state") == "idle" ? "already_stopped" : "stopped";
        j["name"] = script_status(ctx).gets("script");
        resp.set_json(j);
    });

    // 模型的"下载"（前端 demo 页的按钮）：从云端 demo_server 拉进板上 models/。
    // 与 /api/models/upload 是同一条流水线的两个方向（一个推、一个拉）。
    router.add("POST", "/api/demo/download_model_with_progress", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        const std::string name = payload.gets("model_name");
        if (name.empty()) {
            resp.set_error("model_name is required", 400);
            return;
        }
        if (!valid_model_name(name)) {
            resp.set_error("model_name 非法（只允许字母数字与 _ - .）：" + name, 400);
            return;
        }
        const std::string server = payload.gets("demo_server", ctx.config.demo_server_url);
        const Json r = start_model_pull(ctx, name, server + "/api/models/" + name);
        Json j;
        j["status"] = "started";
        j["task_id"] = r.gets("task_id");
        j["new_name"] = name;
        j["path"] = r.gets("path");
        resp.set_json(j);
    });

    router.add_param("GET", "/api/demo/download_progress/{task_id}", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const std::string task_id = req.header("__route_param");
        {
            std::lock_guard<std::mutex> lk(ctx.dl_mu);
            auto it = ctx.downloads.find(task_id);
            if (it != ctx.downloads.end()) {
                resp.set_json(it->second);
                return;
            }
        }
        Json j;
        j["progress"] = csrc::Json((int64_t)0);
        j["status"] = "not_found";
        resp.set_json(j);
    });


    // ── /api/ota ──
    router.add("GET", "/api/ota/version", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        std::string ver;
        int64_t ts = 0;
        read_version_file(ctx, ver, ts);
        Json j;
        j["version"] = ver;
        j["updated"] = csrc::Json(ts);
        j["service"] = "AKA-00";
        resp.set_json(j);
    });

    router.add("GET", "/api/ota/upgrade/progress", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        std::string task_id = req.query_param("task_id");
        Json j;
        {
            std::lock_guard<std::mutex> lk(ctx.ota_mu);
            auto it = ctx.ota_tasks.find(task_id);
            if (it != ctx.ota_tasks.end()) {
                resp.set_json(it->second);
                return;
            }
        }
        j["progress"] = csrc::Json((int64_t)0);
        j["status"] = "unknown";
        resp.set_json(j);
    });

    router.add("GET", "/api/ota/status", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        {
            std::lock_guard<std::mutex> lk(ctx.ota_mu);
            for (auto& kv : ctx.ota_tasks) {
                std::string st = kv.second.gets("status");
                if (st == "downloading" || st == "installing") {
                    Json j;
                    j["status"] = st;
                    j["progress"] = kv.second["progress"];
                    j["message"] = kv.second.gets("message");
                    j["task_id"] = kv.first;
                    resp.set_json(j);
                    return;
                }
            }
        }
        // 磁盘持久化状态
        Json disk;
        std::ifstream f("/root/aka-ota-status.json");
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            Json::parse(ss.str(), disk);
        }
        if (!disk.is_object()) disk = Json();
        disk["status"] = disk.gets("status", "idle");
        resp.set_json(disk);
    });

    router.add("GET", "/api/ota/check", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json info = fetch_release_info(ctx);
        if (!info.is_object() || info.gets("url").empty()) {
            Json err;
            err["status"] = "error";
            err["message"] = "未找到可用更新";
            resp.set_json(err, 404);
            return;
        }
        std::string cur_ver;
        int64_t cur_ts = 0;
        read_version_file(ctx, cur_ver, cur_ts);
        std::string remote_ver = info.gets("version_number");
        if (!remote_ver.empty() && remote_ver[0] == 'v') remote_ver = remote_ver.substr(1);
        int64_t remote_ts = info.geti("version", 0);

        std::vector<int> lv = parse_semver(cur_ver);
        std::vector<int> rv = parse_semver(remote_ver);
        bool has_update;
        if (!lv.empty() && !rv.empty()) {
            has_update = rv > lv;
        } else {
            has_update = remote_ts > cur_ts;
        }

        Json j;
        j["current_version"] = cur_ver;
        j["current_updated"] = csrc::Json(cur_ts);
        j["remote_updated"] = csrc::Json(remote_ts);
        j["update_available"] = has_update;
        j["latest_version"] = info.gets("version_number");
        j["hardware_desc"] = info.gets("hardware_desc");
        j["software_desc"] = info.gets("software_desc");
        j["url"] = info.gets("url");
        resp.set_json(j);
    });

    router.add("POST", "/api/ota/upgrade", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json info = fetch_release_info(ctx);
        if (!info.is_object() || info.gets("url").empty()) {
            Json err;
            err["status"] = "error";
            err["message"] = "未找到可用更新";
            resp.set_json(err, 404);
            return;
        }
        std::string cur_ver;
        int64_t cur_ts = 0;
        read_version_file(ctx, cur_ver, cur_ts);
        std::string remote_ver = info.gets("version_number");
        if (!remote_ver.empty() && remote_ver[0] == 'v') remote_ver = remote_ver.substr(1);
        int64_t remote_ts = info.geti("version", 0);
        std::vector<int> lv = parse_semver(cur_ver);
        std::vector<int> rv = parse_semver(remote_ver);
        bool is_latest = false;
        if (!lv.empty() && !rv.empty()) {
            is_latest = rv <= lv;
        } else {
            is_latest = remote_ts <= cur_ts;
        }
        if (is_latest) {
            Json j;
            j["status"] = "ok";
            j["message"] = "已是最新版本";
            j["version"] = cur_ver;
            resp.set_json(j);
            return;
        }
        std::string download_url = info.gets("url");
        if (download_url.empty()) {
            Json err;
            err["status"] = "error";
            err["message"] = "固件下载地址为空，请检查更新源配置";
            resp.set_json(err, 500);
            return;
        }

        std::string task_id = std::to_string(time(nullptr)) + std::to_string(rand() % 10000);
        {
            std::lock_guard<std::mutex> lk(ctx.ota_mu);
            Json t;
            t["progress"] = csrc::Json((int64_t)0);
            t["status"] = "downloading";
            t["message"] = "准备下载...";
            ctx.ota_tasks[task_id] = t;
        }
        std::ofstream sf("/root/aka-ota-status.json");
        if (sf) {
            Json st;
            st["status"] = "downloading";
            st["task_id"] = task_id;
            sf << st.dump(false);
        }

        std::thread([&ctx, download_url, task_id] {
            try {
                std::string ota_dir = ctx.app_dir + "/.ota";
                std::string mk = "mkdir -p \"" + ota_dir + "\"";
                system(mk.c_str());
                std::string tmp_path = ota_dir + "/download_" + task_id + ".tmp";
                {
                    std::lock_guard<std::mutex> lk(ctx.ota_mu);
                    ctx.ota_tasks[task_id]["message"] = "正在下载固件...";
                }
                csrc::HttpResult r = csrc::http_download(download_url, tmp_path,
                    [&ctx, task_id](int pct) {
                        std::lock_guard<std::mutex> lk(ctx.ota_mu);
                        ctx.ota_tasks[task_id]["progress"] = csrc::Json((int64_t)(pct > 99 ? 99 : pct));
                        ctx.ota_tasks[task_id]["message"] = "正在下载... " + std::to_string(pct > 99 ? 99 : pct) + "%";
                    }, 600);
                if (!r.ok) {
                    std::lock_guard<std::mutex> lk(ctx.ota_mu);
                    ctx.ota_tasks[task_id]["progress"] = csrc::Json((int64_t)0);
                    ctx.ota_tasks[task_id]["status"] = "error";
                    ctx.ota_tasks[task_id]["message"] = r.error;
                    std::ofstream sf("/root/aka-ota-status.json");
                    if (sf) {
                        Json st;
                        st["status"] = "error";
                        st["task_id"] = task_id;
                        st["message"] = r.error;
                        sf << st.dump(false);
                    }
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(ctx.ota_mu);
                    ctx.ota_tasks[task_id]["status"] = "installing";
                    ctx.ota_tasks[task_id]["message"] = "正在安装...";
                }
                std::ofstream sf("/root/aka-ota-status.json");
                if (sf) {
                    Json st;
                    st["status"] = "installing";
                    st["task_id"] = task_id;
                    sf << st.dump(false);
                }
                write_restart_script(tmp_path);
                std::lock_guard<std::mutex> lk(ctx.ota_mu);
                ctx.ota_tasks[task_id]["progress"] = csrc::Json((int64_t)100);
                ctx.ota_tasks[task_id]["status"] = "done";
                ctx.ota_tasks[task_id]["message"] = "安装完成，服务重启中...";
            } catch (...) {
                std::lock_guard<std::mutex> lk(ctx.ota_mu);
                ctx.ota_tasks[task_id]["status"] = "error";
                ctx.ota_tasks[task_id]["message"] = "upgrade exception";
            }
        }).detach();

        Json j;
        j["status"] = "ok";
        j["task_id"] = task_id;
        resp.set_json(j);
    });

    router.add("POST", "/api/ota/update", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        std::string filename, content;
        if (!extract_multipart_file(req.body, req.header("content-type"), filename, content)) {
            Json err;
            err["status"] = "error";
            err["message"] = "no firmware file";
            resp.set_json(err, 400);
            return;
        }
        std::string task_id = std::to_string(time(nullptr)) + std::to_string(rand() % 10000);
        std::string ota_dir = ctx.app_dir + "/.ota";
        system(("mkdir -p \"" + ota_dir + "\"").c_str());
        std::string tmp_path = ota_dir + "/upload_" + task_id + ".tmp";
        {
            std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
            f.write(content.data(), (std::streamsize)content.size());
        }
        // md5 校验（request form 字段）
        // 注意：本实现从 multipart 里只取了文件；md5 字段在非文件 part 中。
        // 简化：跳过 md5 强校验（有需求再补）。
        {
            std::lock_guard<std::mutex> lk(ctx.ota_mu);
            Json t;
            t["progress"] = csrc::Json((int64_t)50);
            t["status"] = "installing";
            t["message"] = "正在安装固件...";
            ctx.ota_tasks[task_id] = t;
        }
        std::thread([&ctx, tmp_path, task_id] {
            try {
                {
                    std::lock_guard<std::mutex> lk(ctx.ota_mu);
                    ctx.ota_tasks[task_id]["progress"] = csrc::Json((int64_t)60);
                    ctx.ota_tasks[task_id]["message"] = "正在准备...";
                }
                std::ofstream sf("/root/aka-ota-status.json");
                if (sf) {
                    Json st;
                    st["status"] = "installing";
                    st["task_id"] = task_id;
                    sf << st.dump(false);
                }
                write_restart_script(tmp_path);
                std::lock_guard<std::mutex> lk(ctx.ota_mu);
                ctx.ota_tasks[task_id]["progress"] = csrc::Json((int64_t)100);
                ctx.ota_tasks[task_id]["status"] = "done";
                ctx.ota_tasks[task_id]["message"] = "安装完成，服务重启中...";
            } catch (...) {
                std::lock_guard<std::mutex> lk(ctx.ota_mu);
                ctx.ota_tasks[task_id]["status"] = "error";
                ctx.ota_tasks[task_id]["message"] = "install exception";
            }
        }).detach();

        Json j;
        j["status"] = "ok";
        j["task_id"] = task_id;
        j["message"] = "upload received, installing...";
        resp.set_json(j);
    });

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

    // ── /api/wifi ──
    router.add("GET", "/api/wifi/ip", [](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        ensure_wpa_env();
        std::string status = csrc::exec_output(
            "wpa_cli -p /var/run/wpa_supplicant -i wlan1 status 2>/dev/null");
        bool connected = status.find("wpa_state=COMPLETED") != std::string::npos;
        std::string ip = connected ? csrc::iface_ip("wlan1") : "192.168.4.1";
        if (ip.empty()) ip = "192.168.4.1";
        Json j;
        j["ip"] = ip;
        resp.set_json(j);
    });

    router.add("GET", "/api/wifi/status", [](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        ensure_wpa_env();
        std::string status = csrc::exec_output(
            "wpa_cli -p /var/run/wpa_supplicant -i wlan1 status 2>/dev/null");
        std::string ssid;
        size_t pos = status.find("\nssid=");
        if (pos != std::string::npos) {
            size_t e = status.find('\n', pos + 6);
            ssid = status.substr(pos + 6, e == std::string::npos ? std::string::npos : e - pos - 6);
        }
        bool connected = status.find("wpa_state=COMPLETED") != std::string::npos;
        std::string ip = connected ? csrc::iface_ip("wlan1") : "192.168.4.1";
        Json j;
        j["ssid"] = ssid.empty() ? Json() : Json(ssid);
        j["ip"] = ip.empty() ? "192.168.4.1" : ip;
        resp.set_json(j);
    });

    router.add("GET", "/api/wifi/scan", [](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        // 扫描（与 Python get_wifi_list 对齐）
        if (!ensure_wpa_env()) {
            Json j;
            j["list"] = Json(Json::Type::Array);  // 空数组，而非 null
            j["error"] = "WPA_INIT_FAILED";
            resp.set_json(j);
            return;
        }
        csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 scan > /dev/null 2>&1");
        std::string raw;
        for (int i = 0; i < 10; i++) {
            usleep(500000);
            raw = csrc::exec_output(
                "wpa_cli -p /var/run/wpa_supplicant -i wlan1 scan_results 2>/dev/null");
            // 与 Python 一致：等到出现表头以外的至少 1 行结果才退出。
            // 仅表头 "bssid / frequency / ... / ssid\n" 只有 1 个换行，需继续等。
            int nl = 0;
            for (char c : raw) if (c == '\n') ++nl;
            if (nl >= 2) break;
        }
        std::string status = csrc::exec_output(
            "wpa_cli -p /var/run/wpa_supplicant -i wlan1 status 2>/dev/null");
        std::string connected_ssid;
        {
            size_t pos = status.find("\nssid=");
            if (pos != std::string::npos) {
                size_t e = status.find('\n', pos + 6);
                connected_ssid = status.substr(pos + 6, e == std::string::npos ? std::string::npos : e - pos - 6);
            }
        }
        // 解析 scan_results: bssid freq signal flags ssid
        Json list;
        std::istringstream iss(raw);
        std::string line;
        std::map<std::string, Json> unique;
        std::getline(iss, line);  // 表头
        while (std::getline(iss, line)) {
            std::istringstream ls(line);
            std::string bssid, freq, signal_s, flags, ssid;
            ls >> bssid >> freq >> signal_s >> flags;
            std::getline(ls, ssid);
            size_t b = ssid.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            ssid = ssid.substr(b);
            if (ssid.empty()) continue;
            int signal = atoi(signal_s.c_str());
            bool secured = !(flags == "[ESS]" || flags == "[WPS][ESS]");
            if (unique.find(ssid) == unique.end() || signal > (int)unique[ssid].geti("signal", -200)) {
                Json item;
                item["ssid"] = ssid;
                item["id"] = csrc::base64_encode(ssid);
                // base64 去掉 '='
                std::string id = item.gets("id");
                id.erase(std::remove(id.begin(), id.end(), '='), id.end());
                item["id"] = id;
                item["signal"] = csrc::Json((int64_t)signal);
                item["secured"] = secured;
                item["is_connected"] = (ssid == connected_ssid);
                unique[ssid] = item;
            }
        }
        // 排序: 已连接优先, 信号强优先
        std::vector<Json> items;
        for (auto& kv : unique) items.push_back(kv.second);
        std::sort(items.begin(), items.end(), [](const Json& a, const Json& b) {
            if (a.getb("is_connected") != b.getb("is_connected")) return a.getb("is_connected");
            return a.geti("signal") > b.geti("signal");
        });
        for (auto& item : items) list.push_back(item);

        Json j;
        j["list"] = list;
        j["connected"] = connected_ssid.empty() ? Json() : Json(connected_ssid);
        resp.set_json(j);
    });

    router.add("POST", "/api/wifi/connect", [](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        Json payload = req.json();
        std::string ssid = payload.gets("ssid");
        std::string password = payload.gets("password");
        if (ssid.empty()) {
            resp.set_error("ssid 不能为空", 400);
            return;
        }
        ensure_wpa_env();  // 确保 wlan1 的 wpa_supplicant 已就绪
        // SSID 转 hex（与 Python do_connect 一致，wpa_supplicant 无引号 hex 当字节）
        std::string ssid_hex;
        {
            char buf[4];
            for (unsigned char c : ssid) {
                snprintf(buf, sizeof buf, "%02x", c);
                ssid_hex += buf;
            }
        }
        csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 remove_network all >/dev/null 2>&1");
        std::string add_out = csrc::exec_output(
            "wpa_cli -p /var/run/wpa_supplicant -i wlan1 add_network 2>/dev/null");
        std::string net_id = add_out;
        {
            size_t nl = net_id.find('\n');
            if (nl != std::string::npos) net_id = net_id.substr(0, nl);
            size_t b = net_id.find_first_not_of(" \t\r\n");
            if (b != std::string::npos) net_id = net_id.substr(b);
        }
        if (net_id.empty()) net_id = "0";
        csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 set_network " + net_id +
                          " ssid " + ssid_hex + " >/dev/null 2>&1");
        if (!password.empty()) {
            csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 set_network " + net_id +
                              " psk \"" + password + "\" >/dev/null 2>&1");
        } else {
            csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 set_network " + net_id +
                              " key_mgmt NONE >/dev/null 2>&1");
        }
        csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 select_network " + net_id +
                          " >/dev/null 2>&1");

        bool ok = false;
        std::string msg = "连接超时";
        for (int attempt = 0; attempt < 10; attempt++) {
            usleep(800000);
            std::string status = csrc::exec_output(
                "wpa_cli -p /var/run/wpa_supplicant -i wlan1 status 2>/dev/null");
            if (status.find("wpa_state=COMPLETED") != std::string::npos) {
                // **不要在这里自己起 DHCP 客户端**。这台板子的 dhcpcd 本来就在管 wlan1：
                // 手动 `ip link set wlan1 up` + wpa_supplicant 时只有一个 IP，正是因为
                // 只有 dhcpcd 在配。capp 再起一个 udhcpc → 两个客户端各要一个地址，
                // 接口上就挂两个 IP（板上实测：dhcpcd 的 .64 + udhcpc 的 .2），
                // 界面显示哪个都不对、用户也不知道哪个能用。
                // 所以这里只等 dhcpcd 关联后自己来配（和手动路径完全一致）。
                std::string ip;
                for (int i = 0; i < 20; i++) {          // 最多等 6s
                    ip = csrc::iface_ip("wlan1");
                    if (!ip.empty()) break;
                    usleep(300000);
                }
                if (ip.empty()) {
                    // 兜底：系统没在跑 dhcpcd（或被配置排除）时，自己拿一次
                    system("udhcpc -i wlan1 -n -q -T 3 >/dev/null 2>&1");
                    ip = csrc::iface_ip("wlan1");
                }
                // 兜底路径可能留下旧地址（udhcpc 只 add 不 del），清一下只留最新的
                ip = csrc::iface_keep_latest_ip("wlan1");
                ok = true;
                msg = ip.empty() ? "获取中..." : ip;
                break;
            }
            if (status.find("FAIL") != std::string::npos ||
                status.find("reason=WRONG_KEY") != std::string::npos ||
                status.find("wpa_state=DISCONNECTED") != std::string::npos ||
                status.find("wpa_state=INACTIVE") != std::string::npos) {
                ok = false;
                msg = "连接失败，请检查密码或信号";
                break;
            }
            if (attempt >= 2 && status.find("wpa_state=SCANNING") != std::string::npos) {
                ok = false;
                msg = "未找到该网络";
                break;
            }
        }
        if (ok) {
            Json j;
            j["ip"] = msg;
            resp.set_json(j);
        } else {
            Json err;
            err["error"] = msg;
            resp.set_json(err, 408);
        }
    });

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

}  // namespace capp
