// csrc/config.cpp — TOML 子集解析器 + Config::load

#include "csrc/config.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#if defined(__linux__)
#include <unistd.h>
#endif

#include "csrc/log.hpp"

namespace csrc {

namespace {

// 简单的 TOML 子集解析：按 [section] 分组，key = value，value 支持 "quoted" / 裸字 / 数字。
// （单行 key=value，无内联表/数组，与项目其它脚本的解析能力对齐）
class TomlReader {
public:
    explicit TomlReader(const std::string& path) {
        std::ifstream f(path);
        if (!f) return;
        std::string line;
        std::string section;
        while (std::getline(f, line)) {
            // 去掉行注释（# 开头或值后 #，值里含 # 的情况本项目不存在）
            size_t hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            // trim
            size_t b = line.find_first_not_of(" \t\r");
            if (b == std::string::npos) continue;
            size_t e = line.find_last_not_of(" \t\r");
            line = line.substr(b, e - b + 1);

            if (!line.empty() && line[0] == '[') {
                size_t close = line.find(']');
                if (close != std::string::npos) {
                    section = line.substr(1, close - 1);
                }
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            if (key.empty()) continue;
            // 去引号
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }
            data_[section + "." + key] = val;
        }
    }

    bool has(const std::string& section, const std::string& key) const {
        return data_.count(section + "." + key) > 0;
    }
    std::string get(const std::string& section, const std::string& key,
                    const std::string& def = "") const {
        auto it = data_.find(section + "." + key);
        return it == data_.end() ? def : it->second;
    }
    int geti(const std::string& section, const std::string& key, int def) const {
        auto it = data_.find(section + "." + key);
        if (it == data_.end()) return def;
        return std::atoi(it->second.c_str());
    }
    double getd(const std::string& section, const std::string& key, double def) const {
        auto it = data_.find(section + "." + key);
        if (it == data_.end()) return def;
        return std::strtod(it->second.c_str(), nullptr);
    }

private:
    static std::string trim(const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r");
        return s.substr(b, e - b + 1);
    }
    std::map<std::string, std::string> data_;
};

}  // namespace

std::string Config::find_config_path() {
    // 1. $AKA_HOME/etc/config.toml（旧布局）
    if (const char* home = std::getenv("AKA_HOME")) {
        std::string p = std::string(home) + "/etc/config.toml";
        std::ifstream f(p);
        if (f.good()) return p;
    }
    // 1b. $AKA_HOME/config.toml（当前打包布局：config.toml 在应用根目录）
    if (const char* home = std::getenv("AKA_HOME")) {
        std::string p = std::string(home) + "/config.toml";
        std::ifstream f(p);
        if (f.good()) return p;
    }
    // 2. 可执行文件 ../etc/config.toml（仅 Linux，/proc/self/exe 可用）
#if defined(__linux__)
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            std::string exe(buf);
            size_t slash = exe.find_last_of('/');
            std::string dir = slash == std::string::npos ? "." : exe.substr(0, slash);
            std::string p = dir + "/../etc/config.toml";
            std::ifstream f(p);
            if (f.good()) return p;
        }
    }
    // 3. CWD config.toml
    {
        std::ifstream f("config.toml");
        if (f.good()) return "config.toml";
    }
#endif  // __linux__
    return "";
}

Config Config::load() {
    Config cfg;
    std::string path = find_config_path();
    if (path.empty()) {
        CAM_WARN("config.toml not found (tried AKA_HOME, ../etc, cwd); using defaults (backend=dev)");
        return cfg;
    }

    TomlReader toml(path);
    cfg.camera.width = toml.geti("camera", "width", cfg.camera.width);
    cfg.camera.height = toml.geti("camera", "height", cfg.camera.height);
    cfg.camera.fps = toml.geti("camera", "fps", cfg.camera.fps);
    cfg.camera.jpeg_quality = toml.geti("camera", "jpeg_quality", cfg.camera.jpeg_quality);

    cfg.motor.backend = toml.get("motor", "backend", cfg.motor.backend);
    cfg.motor.port = toml.get("motor", "port", cfg.motor.port);
    cfg.motor.baudrate = toml.geti("motor", "baudrate", cfg.motor.baudrate);
    cfg.motor.ppr = toml.geti("motor", "ppr", cfg.motor.ppr);

    cfg.arm.backend = toml.get("arm", "backend", cfg.arm.backend);
    cfg.arm.port = toml.get("arm", "port", cfg.arm.port);
    cfg.arm.baudrate = toml.geti("arm", "baudrate", cfg.arm.baudrate);

    cfg.web.port = toml.geti("web", "port", cfg.web.port);
    cfg.web.https_port = toml.geti("web", "https_port", cfg.web.https_port);

    cfg.ota.check_url = toml.get("ota", "check_url", cfg.ota.check_url);

    cfg.chassis.wheel_diameter_mm = toml.getd("chassis", "wheel_diameter_mm", cfg.chassis.wheel_diameter_mm);
    cfg.chassis.gear_ratio = toml.geti("chassis", "gear_ratio", cfg.chassis.gear_ratio);

    cfg.logging.level = toml.get("logging", "level", cfg.logging.level);

    // 环境变量覆盖（与 app/config.py 一致）
    if (const char* v = std::getenv("STATUS_REPORT_URL")) cfg.status_report_url = v;
    if (const char* v = std::getenv("OTA_CHECK_URL")) cfg.ota.check_url = v;

    CAM_INFO("config loaded from %s (motor=%s arm=%s web=%d)",
             path.c_str(), cfg.motor.backend.c_str(), cfg.arm.backend.c_str(), cfg.web.port);
    return cfg;
}

}  // namespace csrc
