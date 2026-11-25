// 速度配置
//
// 入口：register_config_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 对应 app/routes/config.py。

#include "routes_internal.hpp"

#include "csrc/log.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

namespace capp {
namespace routes {

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

// ── 摄像头取流参数（运行时接口）──
//
// 背景：前端"摄像头清晰度"档位过去只能借 POST /api/ota/update 下发一个 sh 脚本改
// config.toml、再删 OTA 锁让 init.sh 重新拉起 capp —— 切一次整机 HTTP 断约 10 秒。
// 这个接口把同一件事做进进程里：
//   1. 改 config.toml：只替换 [camera] 段里的 stream_scale/width/height/quality 四行，
//      其它行（含注释、其它段）原样保留；缺的行补在段末；.tmp + rename 原子落盘。
//   2. 改内存：直接写 ctx.config.camera（camera_cfg_mu 保护）。流线程每帧读一次
//      stream_cfg_snapshot()（见 routes/camera.cpp），所以正在看流的人下一帧就是
//      新尺寸 —— 不重启、不断流，摇杆开着车时切换也不会掉 WS。
// 不在这里改的两项：采集 fps 和 [display] fps_streaming —— 它们跟"清晰度档位"
// 不是一一对应的，现行默认值（采集 30、看流时屏显暂停）对所有档位都成立。

namespace {

/// 去掉行首尾空白（含 \r，板上的文件可能是 CRLF）
std::string trim_ws(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

/// `stream_width = 288` → "stream_width"；不是 key 行返回空串
std::string toml_key_of(const std::string& line) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return "";
    return trim_ws(line.substr(0, eq));
}

/// 把四个 stream_* 写回 config.toml（只动 [camera] 段）。成功返回 true。
///
/// 用整文件行重写而不是正则替换：TOML 里的注释和别的段必须原样保留，
/// 而且板上的文件是用户/OTA 维护的，不希望这个接口"顺手"格式化它。
bool write_stream_cfg_to_toml(const std::string& path, const StreamCfg& v) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<std::string> lines;
    bool crlf = false;   // 原文件是 CRLF 就写回 CRLF（别因为改四个数把整份文件换了行尾）
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); crlf = true; }
        lines.push_back(line);
    }
    in.close();

    const std::vector<std::pair<std::string, std::string>> kv = {
        {"stream_scale", v.scale ? "true" : "false"},
        {"stream_width", std::to_string(v.width)},
        {"stream_height", std::to_string(v.height)},
        {"stream_quality", std::to_string(v.quality)},
    };
    std::vector<bool> done(kv.size(), false);

    // [camera] 段的起止
    size_t sec = lines.size(), end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (trim_ws(lines[i]) == "[camera]") { sec = i; break; }
    }
    if (sec == lines.size()) return false;   // 没这一节就不碰（宁可不改，也不猜）
    for (size_t i = sec + 1; i < lines.size(); ++i) {
        const std::string t = trim_ws(lines[i]);
        if (!t.empty() && t[0] == '[') { end = i; break; }
    }

    for (size_t i = sec + 1; i < end; ++i) {
        const std::string k = toml_key_of(lines[i]);
        for (size_t j = 0; j < kv.size(); ++j) {
            if (k == kv[j].first) {
                lines[i] = kv[j].first + " = " + kv[j].second;
                done[j] = true;
            }
        }
    }
    // 缺的 key 补在段内最后一个非空行之后（别塞到空行/下一段头上）
    size_t insert_at = end;
    while (insert_at > sec + 1 && trim_ws(lines[insert_at - 1]).empty()) --insert_at;
    std::vector<std::string> add;
    for (size_t j = 0; j < kv.size(); ++j) {
        if (!done[j]) add.push_back(kv[j].first + " = " + kv[j].second);
    }
    lines.insert(lines.begin() + (long)insert_at, add.begin(), add.end());

    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (const auto& l : lines) out << l << (crlf ? "\r\n" : "\n");
        if (!out.good()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

/// GET/POST 共用的返回体（锁内取一次一致性快照，别分四次读）
Json camera_cfg_json(AppContext& ctx, const std::string& path) {
    const StreamCfg sc = stream_cfg_snapshot(ctx);
    Json j;
    j["ok"] = true;
    j["stream_scale"] = sc.scale;
    j["stream_width"] = Json((int64_t)sc.width);
    j["stream_height"] = Json((int64_t)sc.height);
    j["stream_quality"] = Json((int64_t)sc.quality);
    j["config_path"] = path;
    return j;
}

/// 范围限制：缩放尺寸 64x36 ~ 1280x720、质量 1~100。
/// width/height 不允许 0（0 在流线程里表示"不缩放"，那是 stream_scale=false 的事，
/// 不该靠一个"开了缩放却没尺寸"的中间态表达）。
bool valid_stream_cfg(const StreamCfg& v, std::string& err) {
    if (v.width < 64 || v.width > 1280) { err = "stream_width 超出范围（64~1280）"; return false; }
    if (v.height < 36 || v.height > 720) { err = "stream_height 超出范围（36~720）"; return false; }
    if (v.quality < 1 || v.quality > 100) { err = "stream_quality 超出范围（1~100）"; return false; }
    return true;
}

/// 档位名只允许 [a-z0-9-] 且 ≤24 字符（它会被写进 camera-profile.json，前端当 key 用）
bool valid_preset(const std::string& preset) {
    if (preset.size() > 24) return false;
    for (char c : preset) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return !preset.empty();
}

/// 档位名（前端的预设 key）落一份给 UI 读：static/camera-profile.json。
/// 只有 preset 字段给了才写（手调数值没有档位名）。
bool write_camera_profile(AppContext& ctx, const std::string& preset, const StreamCfg& v) {
    if (preset.empty()) return true;
    Json p;
    p["preset"] = preset;
    p["width"] = Json((int64_t)v.width);
    p["height"] = Json((int64_t)v.height);
    p["quality"] = Json((int64_t)v.quality);
    p["updated"] = Json((int64_t)time(nullptr));
    const std::string path = ctx.app_dir + "/static/camera-profile.json";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << p.dump(false) << "\n";
        if (!f.good()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

// ── 左右方向切换（底盘电机接线标定）──
//
// 现场问题：部分单体底盘电机接线左右对调，表现为“前进后退正常、只有左右转反”。
// 这里把代理层的 swap_lr 开关暴露给网页：改了立即生效（下发/回读/闭环转向一起 翻），
// 并落 motor_config.json（Python 路径共用同一份，见 services/motor_config.cpp）。

namespace {

/// GET/POST 共用的返回体：以代理层为准（网页改的就是它；非 tt_pid 后端退回配 置值）
Json motor_cfg_json(AppContext& ctx) {
    Json j;
    j["ok"] = true;
    j["swap_lr"] = ctx.motor_link != nullptr ? ctx.motor_link->swap_lr()
                                             : ctx.config.motor.swap_lr;
    return j;
}

}  // namespace

// ── 速度配置 ──

void register_config_routes(Router& router, AppContext& ctx) {


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

    // ── /api/config/camera（前端清晰度档位）──
    //
    // 老固件没有这两个路由 → 前端 cameraProfile.ts 先 GET 探测，404 就回落
    // "OTA 下发脚本 + 重启 capp"那条老路。所以这里的 404/200 就是能力开关。
    router.add("GET", "/api/config/camera", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(camera_cfg_json(ctx, csrc::Config::find_config_path()));
    });

    router.add("POST", "/api/config/camera", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body required", 400);
            return;
        }

        // 缺的字段 = 保持当前值（前端可以只发差量）
        StreamCfg v = stream_cfg_snapshot(ctx);
        if (const Json* p = payload.get("stream_scale")) {
            if (!p->is_bool()) { resp.set_error("stream_scale 须为 true/false", 400); return; }
            v.scale = p->as_int(0) != 0;   // Json 没有 as_bool，is_bool 的 as_int 就是 1/0
        }
        const struct { const char* key; int* dst; } ints[] = {
            {"stream_width", &v.width}, {"stream_height", &v.height}, {"stream_quality", &v.quality},
        };
        for (const auto& f : ints) {
            if (const Json* p = payload.get(f.key)) {
                if (!p->is_number()) { resp.set_error(std::string(f.key) + " 须为数字", 400); return; }
                *f.dst = (int)p->as_int();
            }
        }
        std::string err;
        if (!valid_stream_cfg(v, err)) {
            resp.set_error(err, 400);
            return;
        }
        const std::string preset = payload.gets("preset");
        if (!preset.empty() && !valid_preset(preset)) {
            resp.set_error("preset 非法（只允许 a-z0-9-，≤24 字符）", 400);
            return;
        }

        // 先落盘再改内存：写文件失败就整体失败，不留"改了但不持久"的状态
        const std::string cfg_path = csrc::Config::find_config_path();
        if (cfg_path.empty() || !write_stream_cfg_to_toml(cfg_path, v)) {
            resp.set_error("写 config.toml 失败", 500);
            return;
        }
        if (!write_camera_profile(ctx, preset, v)) {
            resp.set_error("写 camera-profile.json 失败", 500);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(ctx.camera_cfg_mu);
            ctx.config.camera.stream_scale = v.scale;
            ctx.config.camera.stream_width = v.width;
            ctx.config.camera.stream_height = v.height;
            ctx.config.camera.stream_quality = v.quality;
        }
        CAM_INFO("[camera] 取流参数已更新：scale=%d %dx%d q%d（已连接的观众下一帧生效）",
                 v.scale ? 1 : 0, v.width, v.height, v.quality);
        Json out = camera_cfg_json(ctx, cfg_path);
        out["preset"] = preset;
        resp.set_json(out);
    });

    // ── /api/config/motor（左右方向切换）──
    router.add("GET", "/api/config/motor", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(motor_cfg_json(ctx));
    });

    router.add("POST", "/api/config/motor", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body required", 400);
            return;
        }
        const Json* p = payload.get("swap_lr");
        if (p == nullptr || !p->is_bool()) {
            resp.set_error("swap_lr 须为 true/false", 400);
            return;
        }
        const bool v = p->as_int(0) != 0;
        // 先落盘再改内存：写失败就整体失败，不留“改了但不持久”的状态
        if (!save_motor_swap_lr(ctx, v)) {
            resp.set_error("写 motor_config.json 失败", 500);
            return;
        }
        ctx.config.motor.swap_lr = v;
        if (ctx.motor_link != nullptr) ctx.motor_link->set_swap_lr(v);
        CAM_INFO("[motor] 左右方向标定已更新：swap_lr=%d（立即生效，已落 motor_config.json）",
                 v ? 1 : 0);
        resp.set_json(motor_cfg_json(ctx));
    });
}
}  // namespace routes
}  // namespace capp
