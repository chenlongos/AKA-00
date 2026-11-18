// 状态上报
//
// 由 capp/src/services.cpp 按域拆出来（对应 app/services/*.py 的分法）。
// 对应 app/services/status_reporter.py（含 VERSION 读取）。
// 声明都在 capp/context.hpp（那一份是按域分节的伞头文件，调用方只 include 它）。

#include "capp/context.hpp"

#include "csrc/http_client.hpp"
#include "csrc/log.hpp"
#include "csrc/system_utils.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace capp {

namespace {

// "v1.2.3@1722169200" 或 "v1.2.3 1722169200" → 版本号 + 时间戳
void parse_version_line(const std::string& raw, std::string& ver, int64_t& ts) {
    ver = "unknown";
    ts = 0;
    if (raw.empty()) return;
    const char sep = raw.find('@') != std::string::npos ? '@' : ' ';
    const size_t pos = raw.rfind(sep);
    if (pos != std::string::npos) {
        ver = raw.substr(0, pos);
        ts = (int64_t)atoll(raw.substr(pos + 1).c_str());
    } else {
        ver = raw;
    }
}

}  // namespace

// $AKA_HOME 的同级文件路径（发布记录 /root/aka-release、/root/aka-release.pending）。
// 安装器（build-ota.sh）按 `${AKA_HOME%/*}` 同一规则读写，两边必须一致。
std::string release_record_path(AppContext& ctx, const char* name) {
    std::string dir = ctx.app_dir;
    while (!dir.empty() && dir[dir.size() - 1] == '/') dir.erase(dir.size() - 1);
    size_t p = dir.rfind('/');
    std::string base = p == std::string::npos ? std::string(".") : dir.substr(0, p);
    if (base.empty()) base = "/";
    return base + "/" + name;
}

// 包里自带的 VERSION（安装时从 tar 解出来的那份）
void read_package_version_file(AppContext& ctx, std::string& ver, int64_t& ts) {
    std::ifstream f(ctx.app_dir + "/VERSION");
    if (!f) {
        ver = "unknown";
        ts = 0;
        return;
    }
    std::string raw;
    std::getline(f, raw);
    parse_version_line(raw, ver, ts);
}

// 对外版本：优先云端继承记录（aka-release，由安装器在换包成功后落盘），
// 没有才回落到包内 VERSION。这样云端 versionNumber 就是版本判断的主依据，
// 也解决"包内容一样、只改了云端版本号 → 装完版本号不变"的问题。
void read_version_file(AppContext& ctx, std::string& ver, int64_t& ts) {
    ver = "unknown";
    ts = 0;
    std::ifstream f(release_record_path(ctx, "aka-release"));
    if (f) {
        std::string raw;
        std::getline(f, raw);
        parse_version_line(raw, ver, ts);
        if (ver != "unknown") return;
    }
    read_package_version_file(ctx, ver, ts);
}

namespace {

/// 只要版本号（状态上报用；要那个时间戳的走 read_version_file）
std::string read_version(AppContext& ctx) {
    std::string ver;
    int64_t ts = 0;
    read_version_file(ctx, ver, ts);
    return ver;
}


// ── 跨 TU 的控制原语 ──
// 定义必须在 capp 作用域（context.hpp 有声明；脚本宿主 capp/script.cpp 也要用），
// 不能放进上面的匿名 namespace，否则声明与定义分属两个名字，重载解析会歧义。

}  // namespace

void report_status(AppContext& ctx, const std::string& action) {
    if (ctx.config.status_report_url.empty()) return;
    try {
        csrc::RobotStatus s = ctx.collector.get_status();
        csrc::Json robot;
        robot["left_speed"] = std::round(s.left_speed * 100.0) / 100.0;
        robot["right_speed"] = std::round(s.right_speed * 100.0) / 100.0;
        robot["is_moving"] = std::abs(s.left_speed) > 0.01 || std::abs(s.right_speed) > 0.01;
        robot["gripper"] = s.gripper_status;

        csrc::Json cam;
        cam["camera_on"] = ctx.camera_on && ctx.camera.is_available();

        csrc::Json commands;
        {
            std::lock_guard<std::mutex> lk(ctx.cmdlog_mu);
            for (auto& c : ctx.command_log) commands.push_back(c);
        }

        csrc::Json payload;
        payload["cpu"] = csrc::Json((int64_t)csrc::cpu_usage());
        payload["mem"] = csrc::Json((int64_t)csrc::mem_usage());
        payload["disk"] = csrc::Json((int64_t)csrc::disk_usage());
        payload["uptime"] = csrc::Json((int64_t)csrc::uptime_secs());
        payload["version"] = ctx.version;
        payload["robot"] = robot;
        payload["camera"] = cam;
        payload["recent_commands"] = commands;

        csrc::Json body;
        body["action"] = action;
        body["physicalAddress"] = csrc::mac_address("wlan0");
        body["payload"] = payload;

        csrc::HttpResult r = csrc::http_post_json(ctx.config.status_report_url,
                                                  body.dump(false), 10);
        if (!r.ok) {
            CAM_DEBUG("[reporter] report(%s) failed: %s", action.c_str(), r.error.c_str());
        }
    } catch (...) {
        CAM_DEBUG("[reporter] report(%s) exception", action.c_str());
    }
}

void start_status_reporter(AppContext& ctx) {
    ctx.version = read_version(ctx);
    if (ctx.config.status_report_url.empty()) return;
    int interval = 300;
    if (const char* v = std::getenv("STATUS_REPORT_INTERVAL")) interval = atoi(v);
    std::thread([&ctx, interval] {
        report_status(ctx, "boot");
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(interval));
            report_status(ctx, "heartbeat");
        }
    }).detach();
    CAM_INFO("[reporter] status reporter started (interval=%ds)", interval);
}

}  // namespace capp
