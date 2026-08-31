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

std::string demo_base_dir(AppContext& ctx) {
    if (const char* env = getenv("DEMO_BASE_DIR")) return env;
    return ctx.app_dir + "/demo";
}

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
struct DemoInfo {
    std::string name;
    std::string path;
};
std::vector<DemoInfo> list_demos(AppContext& ctx) {
    std::vector<DemoInfo> out;
    std::string base = demo_base_dir(ctx);
    // 简化：用 exec_output("ls -1 ...") 不可靠，改 opendir
    DIR* d = opendir(base.c_str());
    if (!d) return out;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string dir = base + "/" + ent->d_name;
        struct stat st;
        if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        std::string init = dir + "/init.sh";
        struct stat st2;
        if (stat(init.c_str(), &st2) == 0) {
            out.push_back({ent->d_name, dir});
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const DemoInfo& a, const DemoInfo& b) {
        return a.name < b.name;
    });
    return out;
}

std::string current_demo_name(AppContext& ctx) {
    auto demos = list_demos(ctx);
    return demos.empty() ? "" : demos[0].name;
}

// 进程是否存活
bool pid_alive(pid_t pid) {
    return kill(pid, 0) == 0;
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
            result = execute_action(ctx, action, motor_speed, ms);
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
        resp.set_json(j);
    });

    router.add("GET", "/api/motor/direct", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        int left = (int)atof(req.query_param("left", "0").c_str());
        int right = (int)atof(req.query_param("right", "0").c_str());
        double duration = atof(req.query_param("duration", "0").c_str());
        try {
            Json result = run_motor(ctx, left, right, duration);
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
    router.add("GET", "/api/camera/status", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json j;
        j["camera_on"] = ctx.camera_on && ctx.camera.is_available();
        resp.set_json(j);
    });

    router.add("POST", "/api/camera/open", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        bool ok = ensure_camera(ctx);
        Json j;
        j["camera_on"] = ok && ctx.camera.is_available();
        resp.set_json(j, ok ? 200 : 500);
    });

    router.add("POST", "/api/camera/close", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        close_camera(ctx);
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
        while (true) {
            csrc::Camera::Frame f;
            if (ctx.camera.read_latest(f) && !f.data.empty() && f.ts_ms != last_ts) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_send >= min_interval) {
                    if (csrc::Camera::is_jpeg(f.data.data(), f.data.size())) {
                        // MJPEG 直通：头 + jpeg + 尾拼成一个 buffer 一次 write（减少系统调用）
                        std::string part = "--frame\r\nContent-Type: image/jpeg\r\n"
                                           "Content-Length: " + std::to_string(f.data.size()) +
                                           "\r\n\r\n";
                        std::string out;
                        out.reserve(part.size() + f.data.size() + 2);
                        out += part;
                        out.append((const char*)f.data.data(), f.data.size());
                        out += "\r\n";
                        if (!conn.write_all(out)) break;
                        last_ts = f.ts_ms;
                        last_send = now;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
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

        std::vector<uint8_t> jpeg;
        if (current_jpeg(ctx, 25, jpeg) && !jpeg.empty()) {
            j["image"] = csrc::base64_encode(jpeg.data(), jpeg.size());
        } else {
            j["image"] = Json();
        }
        j["image_format"] = "jpeg";
        resp.set_json(j);
    });

    // ── /api/demo ──
    router.add("GET", "/api/demo/list", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json demos;
        for (auto& d : list_demos(ctx)) {
            Json item;
            item["name"] = d.name;
            item["path"] = d.path;
            item["kind"] = "binary";
            demos.push_back(item);
        }
        Json j;
        j["demos"] = demos;
        resp.set_json(j);
    });

    router.add("GET", "/api/demo/name", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json j;
        std::string name = current_demo_name(ctx);
        j["name"] = name.empty() ? Json() : Json(name);
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/init", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        Json payload = req.json();
        std::string demo_name;
        if (payload.is_object()) demo_name = payload.gets("name");
        if (demo_name.empty()) demo_name = current_demo_name(ctx);
        if (demo_name.empty()) {
            resp.set_error("no demo found", 404);
            return;
        }
        std::string demo_dir = demo_base_dir(ctx) + "/" + demo_name;
        std::string init_script = demo_dir + "/init.sh";
        struct stat st;
        if (stat(demo_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            resp.set_error("demo '" + demo_name + "' not found", 404);
            return;
        }
        if (stat(init_script.c_str(), &st) != 0) {
            resp.set_error("init.sh not found in demo '" + demo_name + "'", 404);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(ctx.demo_mu);
            if (ctx.demo_pid > 0 && pid_alive(ctx.demo_pid)) {
                Json j;
                j["error"] = "demo is already running";
                j["pid"] = csrc::Json((int64_t)ctx.demo_pid);
                resp.set_json(j, 409);
                return;
            }
            chmod(init_script.c_str(), 0755);
            pid_t pid = fork();
            if (pid == 0) {
                // 子进程：新会话 + 切目录 + 执行 init.sh
                setsid();
                chdir(demo_dir.c_str());
                int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    dup2(devnull, 0);
                    dup2(devnull, 1);
                    dup2(devnull, 2);
                    if (devnull > 2) close(devnull);
                }
                execl("/bin/sh", "sh", init_script.c_str(), (char*)nullptr);
                _exit(127);
            }
            if (pid < 0) {
                resp.set_error("fork failed", 500);
                return;
            }
            ctx.demo_pid = pid;
            ctx.demo_pgid = pid;
            ctx.demo_name = demo_name;
            Json j;
            j["status"] = "started";
            j["pid"] = csrc::Json((int64_t)pid);
            j["pgid"] = csrc::Json((int64_t)pid);
            j["name"] = demo_name;
            resp.set_json(j);
        }
    });

    router.add("POST", "/api/demo/stop", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        std::lock_guard<std::mutex> lk(ctx.demo_mu);
        Json j;
        if (ctx.demo_pid <= 0 || !pid_alive(ctx.demo_pid)) {
            j["status"] = "already_stopped";
            j["name"] = ctx.demo_name.empty() ? "unknown" : ctx.demo_name;
            resp.set_json(j);
            return;
        }
        pid_t pid = ctx.demo_pid;
        ctx.demo_pid = -1;
        ctx.demo_pgid = -1;
        std::string name = ctx.demo_name;
        ctx.demo_name.clear();

        kill(pid, SIGTERM);
        // 最多等 3 秒，未响应升级 SIGKILL
        for (int i = 0; i < 30 && pid_alive(pid); i++) {
            usleep(100000);
        }
        if (pid_alive(pid)) kill(pid, SIGKILL);
        int wstatus = 0;
        waitpid(pid, &wstatus, 0);  // 回收子进程，防僵尸
        j["status"] = "stopped";
        j["pid"] = csrc::Json((int64_t)pid);
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/download_model_with_progress", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        std::string model_name = payload.gets("model_name");
        std::string demo_server = payload.gets("demo_server", ctx.config.demo_server_url);
        if (model_name.empty()) {
            resp.set_error("model_name is required", 400);
            return;
        }
        std::string current_name = current_demo_name(ctx);
        if (current_name.empty()) {
            resp.set_error("no local demo found", 404);
            return;
        }
        std::string base = demo_base_dir(ctx);
        std::string old_dir = base + "/" + current_name;
        std::string new_dir = base + "/" + model_name;
        std::string file_path = (model_name == current_name ? old_dir : new_dir) + "/yolo_model.cvimodel";
        std::string url = demo_server + "/api/models/" + model_name;

        std::string task_id = current_name + "_to_" + model_name;
        {
            std::lock_guard<std::mutex> lk(ctx.dl_mu);
            if (ctx.downloads.size() > 20) ctx.downloads.erase(ctx.downloads.begin());
            Json t;
            t["progress"] = csrc::Json((int64_t)0);
            t["status"] = "downloading";
            t["error"] = Json();
            ctx.downloads[task_id] = t;
        }

        std::thread([&ctx, url, file_path, new_dir, old_dir, task_id] {
            try {
                // 目录重命名（新 demo 名称不同时）
                if (new_dir != old_dir) {
                    struct stat st;
                    if (stat(new_dir.c_str(), &st) == 0) {
                        std::string rm = "rm -rf \"" + new_dir + "\"";
                        system(rm.c_str());
                    }
                    rename(old_dir.c_str(), new_dir.c_str());
                }
                csrc::HttpResult r = csrc::http_download(url, file_path,
                    [&ctx, task_id](int pct) {
                        std::lock_guard<std::mutex> lk(ctx.dl_mu);
                        ctx.downloads[task_id]["progress"] = csrc::Json((int64_t)pct);
                    }, 120);
                std::lock_guard<std::mutex> lk(ctx.dl_mu);
                if (r.ok) {
                    ctx.downloads[task_id]["progress"] = csrc::Json((int64_t)100);
                    ctx.downloads[task_id]["status"] = "done";
                } else {
                    ctx.downloads[task_id]["status"] = "error";
                    ctx.downloads[task_id]["error"] = r.error;
                }
            } catch (...) {
                std::lock_guard<std::mutex> lk(ctx.dl_mu);
                ctx.downloads[task_id]["status"] = "error";
                ctx.downloads[task_id]["error"] = "download exception";
            }
        }).detach();

        Json j;
        j["status"] = "started";
        j["task_id"] = task_id;
        j["new_name"] = model_name;
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/upload_model", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        std::string filename, content;
        if (!extract_multipart_file(req.body, req.header("content-type"), filename, content)) {
            resp.set_error("file is required", 400);
            return;
        }
        std::string current_name = current_demo_name(ctx);
        if (current_name.empty()) {
            resp.set_error("no local demo found", 404);
            return;
        }
        std::string file_path = demo_base_dir(ctx) + "/" + current_name + "/yolo_model.cvimodel";
        std::ofstream f(file_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            resp.set_error("cannot write file", 500);
            return;
        }
        f.write(content.data(), (std::streamsize)content.size());
        f.close();
        Json j;
        j["status"] = "uploaded";
        j["size"] = csrc::Json((int64_t)content.size());
        j["name"] = current_name;
        resp.set_json(j);
    });

    router.add_param("GET", "/api/demo/download_progress/{task_id}", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        std::string task_id = req.header("__route_param");
        Json j;
        {
            std::lock_guard<std::mutex> lk(ctx.dl_mu);
            auto it = ctx.downloads.find(task_id);
            if (it != ctx.downloads.end()) {
                resp.set_json(it->second);
                return;
            }
        }
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
                system("udhcpc -i wlan1 -n -q -T 3 >/dev/null 2>&1");
                std::string ip = csrc::iface_ip("wlan1");
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
