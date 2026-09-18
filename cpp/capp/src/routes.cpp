// capp/routes.cpp — 全部 HTTP 路由处理器
//
// 对应 app/routes/*.py（API 契约与 frontend/src/api.ts 完全对齐）
// 包含: control / motor / arm / camera / demo / ota / system / wifi / config / frontend

#include "capp/http_server.hpp"

#include <algorithm>
#include <cctype>
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

// ── demo 卡片（**动作 × 模型**）──
// 一张卡片 = 一份 JSON：$AKA_HOME/demo/configs/<卡片名>.json
//     {"action":"approach","model":"tennis","target_size":300,"speed":30,...}
// 卡片由**用户在界面上新建**（选动作 + 选模型 + 填参数），接口是 POST /api/demo/config。
// 卡片名**只当文件名用**（可以是中文「追网球接近」），动作和模型写在文件里 ——
// 所以不需要"从名字拆出模型和动作"（那种拆法遇到模型名自带 `-` 就歧义了）。
// 跑卡片时：读配置 → 跑 demo/<动作>.lua → 把 params.model 注入成**配置里的模型**。
// （注意不是卡片名！搞错的话会变成"注册模型失败：…/demo/models/追网球接近.cvimodel"）
std::string demo_config_dir(AppContext& ctx) { return ctx.app_dir + "/demo/configs"; }

std::string demo_config_path(AppContext& ctx, const std::string& name) {
    return demo_config_dir(ctx) + "/" + name + ".json";
}

constexpr int kDemoTargetSizeDefault = 300;
constexpr int kDemoSpeedDefault = 25;        // 直线速度（%）
constexpr int kDemoTurnSpeedDefault = 25;    // 转弯速度（%）—— 和直线分开：转弯要的占空比不同
// 执行方式（卡片上一个字段）：跑一遍就结束 / 跑完接着跑直到被停
constexpr const char* kDemoModeDefault = "once";

/// 一张卡片（= 一份 configs/<卡片名>.json）
struct DemoCard {
    std::string name;     // 卡片名（= 文件名，可能中文）
    std::string action;   // 动作 = 脚本名（demo/<action>.lua）
    std::string model;    // 模型（demo/models/<model>.cvimodel）
    csrc::Json params;    // 四个运行参数（缺的用默认值兜底）
};

/// 读一张卡片；不存在 / 解析失败 / 名字非法 / 没写 action 或 model → 返回 false
bool load_demo_card(AppContext& ctx, const std::string& name, DemoCard& out) {
    if (!valid_card_name(name)) return false;
    std::ifstream f(demo_config_path(ctx, name));
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    csrc::Json one;
    if (!csrc::Json::parse(ss.str(), one) || !one.is_object()) return false;

    out.name = name;
    out.action = one.gets("action");
    out.model = one.gets("model");
    out.params = csrc::Json();
    out.params["target_size"] = csrc::Json((int64_t)one.geti("target_size", kDemoTargetSizeDefault));
    out.params["speed"] = csrc::Json((int64_t)one.geti("speed", kDemoSpeedDefault));
    out.params["turn_speed"] = csrc::Json((int64_t)one.geti("turn_speed", kDemoTurnSpeedDefault));
    const std::string mode = one.gets("mode");
    out.params["mode"] = (mode == "loop") ? "loop" : "once";
    return !out.action.empty() && !out.model.empty();
}

/// 列出所有卡片：扫 demo/configs/*.json（= 卡片就是配置，没有单独的注册表）
std::vector<DemoCard> list_demo_cards(AppContext& ctx) {
    std::vector<DemoCard> out;
    const std::string dir = demo_config_dir(ctx);
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() <= 5 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
        DemoCard c;
        if (load_demo_card(ctx, n.substr(0, n.size() - 5), c)) out.push_back(c);
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const DemoCard& x, const DemoCard& y) { return x.name < y.name; });
    return out;
}

/// 写一张卡片（新建或覆盖）
bool save_demo_card(AppContext& ctx, const std::string& name, const std::string& action,
                    const std::string& model, const csrc::Json& params) {
    if (!valid_card_name(name)) return false;
    // demo/configs/ 可能还不存在（板上第一次建卡时），而 ofstream 不会建目录
    if (!csrc::ensure_dir(demo_config_dir(ctx))) return false;
    csrc::Json one;
    one["action"] = action;
    one["model"] = model;
    one["target_size"] = csrc::Json((int64_t)params.geti("target_size", kDemoTargetSizeDefault));
    one["speed"] = csrc::Json((int64_t)params.geti("speed", kDemoSpeedDefault));
    one["turn_speed"] = csrc::Json((int64_t)params.geti("turn_speed", kDemoTurnSpeedDefault));
    one["mode"] = (params.gets("mode") == "loop") ? "loop" : "once";
    std::ofstream f(demo_config_path(ctx, name));
    if (!f) return false;
    f << one.dump(false);
    f.close();
    return (bool)f;
}

/// 动作清单：扫 demo/<动作>.lua（`_` 开头的跳过 —— 那是模板/草稿，不是一个动作）。
/// 显示名取脚本第一行的约定注释 `-- name: 接近瞄准`；没有就用文件名。
struct ActionInfo {
    std::string id;
    std::string name;
};

std::vector<ActionInfo> list_actions(AppContext& ctx) {
    std::vector<ActionInfo> out;
    const std::string dir = ctx.app_dir + "/demo";
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() <= 4 || n.compare(n.size() - 4, 4, ".lua") != 0) continue;
        if (n[0] == '_') continue;                        // 下划线开头的是模板/草稿，不算一个动作
        const std::string id = n.substr(0, n.size() - 4);
        if (!valid_model_name(id)) continue;              // 动作名要能拼进路径
        ActionInfo a;
        a.id = id;
        a.name = id;
        std::ifstream f(dir + "/" + n);
        std::string first;
        if (f && std::getline(f, first)) {
            const std::string key = "name:";
            const size_t at = first.find(key);
            if (at != std::string::npos) {
                std::string label = first.substr(at + key.size());
                const size_t b = label.find_first_not_of(" \t");
                const size_t e2 = label.find_last_not_of(" \t\r");
                if (b != std::string::npos) a.name = label.substr(b, e2 - b + 1);
            }
        }
        out.push_back(a);
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const ActionInfo& x, const ActionInfo& y) { return x.id < y.id; });
    return out;
}

/// 模型清单：扫 demo/models/*.cvimodel（"新建卡片"的下拉要用）
std::vector<std::string> list_models(AppContext& ctx) {
    std::vector<std::string> out;
    const std::string dir = model_dir(ctx);
    const std::string suffix = ".cvimodel";
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() <= suffix.size() ||
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        out.push_back(n.substr(0, n.size() - suffix.size()));
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
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

// multipart/form-data 解析（upload_model / OTA update / 训练平台直传模型 用）
//
// 一个 part 的三样东西：字段名 name、文件名 filename（文件字段才有）、内容 content。
// 注意**不能只取第一个 part**：训练平台的表单是 file + name 两个字段，先来哪个不确定。
struct MultipartPart {
    std::string name;
    std::string filename;
    std::string content;
};

std::vector<MultipartPart> parse_multipart(const std::string& body,
                                           const std::string& content_type) {
    std::vector<MultipartPart> out;
    size_t bpos = content_type.find("boundary=");
    if (bpos == std::string::npos) return out;
    std::string boundary = content_type.substr(bpos + 9);
    if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"') {
        boundary = boundary.substr(1, boundary.size() - 2);
    }
    const size_t semi = boundary.find(';');   // 有的客户端会写 boundary=xxx; charset=...
    if (semi != std::string::npos) boundary = boundary.substr(0, semi);
    if (boundary.empty()) return out;

    const std::string delim = "--" + boundary;
    size_t cursor = 0;
    while (true) {
        const size_t b = body.find(delim, cursor);
        if (b == std::string::npos) break;
        size_t after = b + delim.size();
        if (body.compare(after, 2, "--") == 0) break;      // 收尾的 --boundary--
        if (body.compare(after, 2, "\r\n") == 0) after += 2;
        const size_t hdr_end = body.find("\r\n\r\n", after);
        if (hdr_end == std::string::npos) break;
        const std::string headers = body.substr(after, hdr_end - after);
        const size_t data_start = hdr_end + 4;
        const size_t next = body.find(delim, data_start);
        size_t data_end = (next == std::string::npos) ? body.size() : next;
        if (data_end >= 2 && body.compare(data_end - 2, 2, "\r\n") == 0) data_end -= 2;

        MultipartPart p;
        const size_t nm = headers.find("name=\"");
        if (nm != std::string::npos) {
            const size_t e = headers.find('"', nm + 6);
            if (e != std::string::npos) p.name = headers.substr(nm + 6, e - (nm + 6));
        }
        const size_t fn = headers.find("filename=\"");
        if (fn != std::string::npos) {
            const size_t e = headers.find('"', fn + 10);
            if (e != std::string::npos) p.filename = headers.substr(fn + 10, e - (fn + 10));
        }
        p.content = body.substr(data_start, data_end - data_start);
        out.push_back(std::move(p));
        if (next == std::string::npos) break;
        cursor = data_end;
    }
    return out;
}

/// 取第一个 part 当文件（老的调用方：平台推模型、OTA 传固件，都不关心字段名）
bool extract_multipart_file(const std::string& body, const std::string& content_type,
                            std::string& filename, std::string& content) {
    const std::vector<MultipartPart> parts = parse_multipart(body, content_type);
    if (parts.empty()) return false;
    if (!parts[0].filename.empty()) filename = parts[0].filename;
    content = parts[0].content;
    return true;
}

/// 扩展名是不是 .cvimodel（大小写不敏感）
bool has_cvimodel_ext(const std::string& filename) {
    if (filename.size() < 9) return false;
    std::string tail = filename.substr(filename.size() - 9);
    for (char& c : tail) c = (char)tolower((unsigned char)c);
    return tail == ".cvimodel";
}

/// 去掉首尾空白（multipart 文本字段带不带换行看客户端，不能想当然）
std::string trim_ws(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
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
    // 模型必填（裸名字 → $AKA_HOME/demo/models/<名字>.cvimodel）；可选阈值 ?conf=&iou=
    // （不给用默认 0.25 / 0.45，给错值直接 400）。
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
        // 可选阈值：?conf=0.6&iou=0.3（不给就用默认 0.25 / 0.45 = 与以前完全一样）。
        // 给错值直接报错而不是悄悄用默认 —— 调参时最怕"以为生效了其实没生效"。
        double conf = 0, iou = 0;   // 0 = 没给，交给 decode_options 取默认
        auto read_thresh = [&](const char* key, double& out) -> bool {
            const std::string v = req.query_param(key);
            if (v.empty()) return true;
            char* end = nullptr;
            const double d = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || d <= 0 || d >= 1) return false;
            out = d;
            return true;
        };
        if (!read_thresh("conf", conf) || !read_thresh("iou", iou)) {
            Json j;
            j["ok"] = false;
            j["error"] = "conf / iou 要在 0~1 之间（如 ?conf=0.6&iou=0.3）；不给就用默认 0.25 / 0.45";
            resp.set_json(j, 400);
            return;
        }
        const Json j = detect_once(ctx, model, decode_options(conf, iou));
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

    // ── 训练平台直传模型（浏览器 → 小车，同一局域网；yolotrain.chenlongrobot.com）──
    //
    // 与上面 `/api/models/upload` 的区别：那个是"平台/curl 推模型"（名字走 query，body 就是
    // 文件裸内容，响应 {ok,name,path,size}）；这个是**浏览器表单直传**（multipart 两个字段
    // file+name，响应 {status,name,size}）。
    // **不再给模型生成脚本**：动作脚本是预定义、与模型无关的，传完模型后在 Demo 页建一张卡
    // （动作 × 这个模型），或直接 POST /api/demo/init {"action":"grab","model":"<名字>"}。
    //
    // CORS 与 OPTIONS 预检不在这里处理：http_server 在路由之前就统一应答了（所有响应也
    // 自动带 Access-Control-Allow-Origin: *），浏览器跨域直传本来就要求那样。
    router.add("POST", "/api/model/upload", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        auto fail = [&resp](const std::string& msg) {
            Json e;
            e["status"] = "error";
            e["message"] = msg;
            resp.set_json(e, 400);
        };

        const std::string ct = req.header("content-type");
        std::string filename, content, name;
        if (ct.find("multipart/form-data") != std::string::npos) {
            for (const auto& p : parse_multipart(req.body, ct)) {
                // 字段名以表单为准；两个字段谁先到不确定，所以是遍历而不是取第一个 part
                if (p.name == "file") {
                    filename = p.filename;
                    content = p.content;
                } else if (p.name == "name") {
                    name = trim_ws(p.content);
                }
            }
        }
        if (name.empty()) name = trim_ws(req.query_param("name"));   // ?name= 兜底

        // 逐条按契约校验，失败一律 400 + {status:"error", message}
        if (content.empty()) {
            fail("invalid file");
            return;
        }
        if (!has_cvimodel_ext(filename)) {   // 后缀不对 = 发错文件了（平台固定发 model.cvimodel）
            fail("invalid file");
            return;
        }
        if (name.empty()) {
            fail("invalid name");
            return;
        }
        if (!valid_model_name(name)) {   // 名字要拼进路径：`../../etc/passwd` 挡在这里
            fail("invalid name");
            return;
        }

        const Json r = save_model_upload(ctx, name, content);
        if (!r.getb("ok")) {
            fail(r.gets("error"));   // 魔数不对 / 过大 / 换入失败 —— 原因比"invalid file"有用
            return;
        }
        Json j;
        j["status"] = "ok";
        j["name"] = name;
        j["size"] = Json((int64_t)r.geti("size", 0));
        j["path"] = r.gets("path");
        // script / script_created：训练平台那份契约里的字段，**保留不删**（平台在读），
        // 但语义变了 —— 动作脚本是预定义的、与模型无关，上传模型不再生成脚本。
        // 模型传上来就能用：建一张卡片（动作 × 这个模型）或直接
        // POST /api/demo/init {"action":"grab","model":"<名字>"}。
        j["script"] = "";
        j["script_created"] = false;
        // 顺手把可用的动作清单带上，平台侧想提示"能用哪些动作"就有数据了
        Json actions(Json::Type::Array);
        for (const auto& a : list_actions(ctx)) actions.push_back(a.id);
        j["actions"] = actions;
        resp.set_json(j);
    });

    // ── 动作脚本（demo/*.lua）── 接口都挂在 /api/demo 下（跟卡片/配置同一套命名）
    //
    // `/api/demo/run` 是**最底层**的那条：直接跑某个动作脚本 + 任意 params（不校验模型），
    // 调试/一次性用；正常跑 demo 走 `/api/demo/init`（跑卡片，或 action+model，会先校验
    // 动作脚本和模型文件都在）。
    // 把"看→对准→靠近→抓"这类要反复调参的流程写成脚本，改一行存盘重跑，不用重编部署。
    // 安全兜底（限速/被人的指令取代/底盘掉线/内存与卡死）全在宿主里，脚本绕不过去。
    router.add("POST", "/api/demo/run", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        const std::string name = payload.gets("script");
        if (name.empty()) {
            resp.set_error("script 必填（动作名，例：grab）", 400);
            return;
        }
        // params 原样给脚本（含 mode=once|loop）；**没有 max_seconds**，跑多久看模式与停止
        const Json* params = payload.get("params");
        const Json r = script_run(ctx, name, params ? *params : Json());
        resp.set_json(r, r.getb("ok") ? 200 : 400);
    });

    router.add("GET", "/api/demo/status", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(script_status(ctx));
    });


    // ── 屏显示开关 ──
    // 为什么要有：全屏写屏很吃那颗单核 CPU（实测 /api/detect 从 120ms 涨到 340ms），
    // 要在跑检测/追物时让出 CPU 就把它关掉；想看屏就再打开。
    // 只改运行时状态，不写 config.toml（重启后回到文件里的值）。
    router.add("GET", "/api/display/status", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(display_config(ctx));
    });

    router.add("POST", "/api/display/enabled", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        // 也接受 ?enabled=0/1（前端表单有时更顺手）
        const std::string q = req.query_param("enabled");
        bool enabled;
        if (!q.empty()) {
            enabled = (q == "1" || q == "true" || q == "yes");
        } else if (payload.get("enabled")) {
            enabled = payload.getb("enabled", true);
        } else {
            resp.set_error("enabled 必填（true/false）", 400);
            return;
        }
        resp.set_json(set_display_enabled(ctx, enabled));
    });

    // ── /api/demo ── 一张卡片 = **动作 × 模型**（用户在界面上新建，见文件上方 DemoCard 的说明）
    //
    // 前端契约：卡片名仍然是 name（前端拿它当 key 与显示），另给 action/model/action_name；
    // "新建卡片"要用的动作清单与模型清单也跟着 list 一起回，省一次请求。
    router.add("GET", "/api/demo/list", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json demos(Json::Type::Array);
        for (const auto& c : list_demo_cards(ctx)) {
            const bool has_action = action_script_exists(ctx, c.action);
            const bool has_model = access(model_path(ctx, c.model).c_str(), F_OK) == 0;
            Json item;
            item["name"] = c.name;                                  // 卡片名（前端只认这个）
            item["action"] = c.action;
            item["model"] = c.model;
            item["script"] = has_action ? c.action : "";            // 兼容老字段：动作名
            item["path"] = has_model ? model_path(ctx, c.model) : "";
            item["kind"] = "card";
            // 动作脚本或模型文件缺了也照样列出来 —— 点开始会明确报错，别让卡片凭空消失
            item["ready"] = has_action && has_model;
            item["error"] = !has_action ? ("动作脚本缺失：demo/" + c.action + ".lua")
                          : (!has_model ? ("模型文件缺失：demo/models/" + c.model + ".cvimodel") : "");
            demos.push_back(item);
        }
        Json actions(Json::Type::Array);
        for (const auto& a : list_actions(ctx)) {
            Json x;
            x["id"] = a.id;
            x["name"] = a.name;      // 脚本第一行 `-- name: 接近瞄准` 给的显示名
            actions.push_back(x);
        }
        Json models(Json::Type::Array);
        for (const auto& m : list_models(ctx)) models.push_back(m);
        Json j;
        j["demos"] = demos;
        j["actions"] = actions;
        j["models"] = models;
        resp.set_json(j);
    });

    router.add("GET", "/api/demo/name", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json st = script_status(ctx);
        Json j;
        j["name"] = st.gets("card");       // 跑的是哪张卡片
        j["action"] = st.gets("script");   // 动作脚本名
        j["model"] = st.gets("model");
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/init", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }

        // 两种调用方式：
        //   ① {"name":"追网球接近"}                       ← 界面点"开始"（跑存下来的那张卡片）
        //   ② {"action":"approach","model":"tennis",...}  ← 直接跑，不用建卡
        //      （"刚传上来一个新模型，立刻用它接近一下"就是这条）
        std::string action, model, card;
        Json params;
        const std::string name = payload.gets("name");
        if (!name.empty()) {
            if (!valid_card_name(name)) {
                resp.set_error("卡片名非法：" + name, 400);
                return;
            }
            DemoCard c;
            if (!load_demo_card(ctx, name, c)) {
                resp.set_error("没有这张卡片（或配置读不了）：demo/configs/" + name + ".json", 400);
                return;
            }
            action = c.action;
            model = c.model;
            card = name;
            params = c.params;
        } else {
            action = payload.gets("action");
            model = payload.gets("model");
            if (action.empty() || model.empty()) {
                resp.set_error("要么给 name（跑已建的卡片），要么给 action + model（直接跑）", 400);
                return;
            }
            params["target_size"] = Json((int64_t)payload.geti("target_size", kDemoTargetSizeDefault));
            params["speed"] = Json((int64_t)payload.geti("speed", kDemoSpeedDefault));
            params["turn_speed"] = Json((int64_t)payload.geti("turn_speed", kDemoTurnSpeedDefault));
            params["mode"] = (payload.gets("mode") == "loop") ? "loop" : "once";
        }

        // 名字都要拼进路径，且必须真存在 —— 在这里挡掉，别让它变成脚本里一句含糊的报错
        if (!valid_model_name(action)) {
            resp.set_error("动作名非法（只允许字母数字与 _ - .）：" + action, 400);
            return;
        }
        if (!valid_model_name(model)) {
            resp.set_error("模型名非法（只允许字母数字与 _ - .）：" + model, 400);
            return;
        }
        if (!action_script_exists(ctx, action)) {
            resp.set_error("动作脚本不存在：demo/" + action + ".lua", 400);
            return;
        }
        if (access(model_path(ctx, model).c_str(), F_OK) != 0) {
            resp.set_error("模型不存在：demo/models/" + model + ".cvimodel", 400);
            return;
        }

        // 请求里显式传的参数优先（卡片里那份作底）
        if (payload.get("target_size")) params["target_size"] = Json(payload.geti("target_size", kDemoTargetSizeDefault));
        if (payload.get("speed")) params["speed"] = Json(payload.geti("speed", kDemoSpeedDefault));
        if (payload.get("turn_speed")) params["turn_speed"] = Json(payload.geti("turn_speed", kDemoTurnSpeedDefault));
        if (payload.get("mode")) params["mode"] = payload.gets("mode");

        // ★ 模型来自卡片/请求，**不是卡片名** —— 搞错的话脚本会去开
        //   demo/models/<卡片名>.cvimodel，报错长成"注册模型失败"，极具误导性
        params["model"] = model;
        params["card"] = card;   // 让状态能回答"现在跑的是哪张卡"；脚本不用管它

        const Json r = script_run(ctx, action, params);

        if (!r.getb("ok")) {
            const Json st = script_status(ctx);
            Json j;
            j["status"] = "already_running";
            j["pid"] = Json((int64_t)getpid());
            j["name"] = st.gets("card");
            j["error"] = r.gets("error");
            resp.set_json(j, 409);
            return;
        }
        Json j;
        j["status"] = "started";
        j["name"] = card.empty() ? action : card;   // 卡片名（没建卡直接跑时回动作名）
        j["script"] = action;
        j["action"] = action;
        j["model"] = model;
        j["pid"] = Json((int64_t)getpid());   // 兼容字段：跑 demo 的进程就是 capp 自己
        j["pgid"] = Json((int64_t)getpid());
        resp.set_json(j);
    });

    // 卡片配置：GET 读一张、POST 新建或覆盖（动作 + 模型 + 四个参数）
    router.add("GET", "/api/demo/config", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const std::string name = req.query_param("name");
        if (name.empty()) {
            resp.set_error("name 必填（卡片名，如 ?name=追网球接近）", 400);
            return;
        }
        if (!valid_card_name(name)) {
            resp.set_error("卡片名非法（不能含 / \\ 与控制字符，不能以 . 开头）：" + name, 400);
            return;
        }
        DemoCard c;
        if (!load_demo_card(ctx, name, c)) {
            resp.set_error("没有这张卡片（或配置读不了）：demo/configs/" + name + ".json", 400);
            return;
        }
        Json j = c.params;
        j["name"] = c.name;
        j["action"] = c.action;
        j["model"] = c.model;
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/config", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        const std::string name = payload.gets("name");
        if (name.empty()) {
            resp.set_error("name 必填（卡片名，如 追网球接近）", 400);
            return;
        }
        if (!valid_card_name(name)) {
            resp.set_error("卡片名非法（不能含 / \\ 与控制字符，不能以 . 开头）：" + name, 400);
            return;
        }
        const std::string action = payload.gets("action");
        const std::string model = payload.gets("model");
        if (action.empty() || model.empty()) {
            resp.set_error("action 与 model 必填（这张卡片跑哪个动作、用哪个模型）", 400);
            return;
        }
        if (!valid_model_name(action)) {
            resp.set_error("动作名非法（只允许字母数字与 _ - .）：" + action, 400);
            return;
        }
        if (!valid_model_name(model)) {
            resp.set_error("模型名非法（只允许字母数字与 _ - .）：" + model, 400);
            return;
        }
        if (!action_script_exists(ctx, action)) {
            resp.set_error("动作脚本不存在：demo/" + action + ".lua", 400);
            return;
        }
        if (access(model_path(ctx, model).c_str(), F_OK) != 0) {
            resp.set_error("模型不存在：demo/models/" + model + ".cvimodel", 400);
            return;
        }
        if (!save_demo_card(ctx, name, action, model, payload)) {
            resp.set_error("写入 demo/configs/" + name + ".json 失败", 500);
            return;
        }
        Json j;
        j["ok"] = true;
        j["name"] = name;
        j["action"] = action;
        j["model"] = model;
        j["target_size"] = Json((int64_t)payload.geti("target_size", kDemoTargetSizeDefault));
        j["speed"] = Json((int64_t)payload.geti("speed", kDemoSpeedDefault));
        j["turn_speed"] = Json((int64_t)payload.geti("turn_speed", kDemoTurnSpeedDefault));
        j["mode"] = (payload.gets("mode") == "loop") ? "loop" : "once";
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/delete", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        const std::string name = payload.is_object() ? payload.gets("name") : "";
        if (name.empty()) {
            resp.set_error("name 必填（要删的卡片名）", 400);
            return;
        }
        if (!valid_card_name(name)) {
            resp.set_error("卡片名非法：" + name, 400);
            return;
        }
        const std::string path = demo_config_path(ctx, name);
        if (std::remove(path.c_str()) != 0) {
            resp.set_error("没有这张卡片（或删不掉）：demo/configs/" + name + ".json", 400);
            return;
        }
        Json j;
        j["ok"] = true;
        j["name"] = name;
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/stop", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json r = script_stop(ctx);
        const Json st = script_status(ctx);
        Json j;
        j["status"] = r.gets("state") == "idle" ? "already_stopped" : "stopped";
        j["name"] = st.gets("card");
        j["action"] = st.gets("script");
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
