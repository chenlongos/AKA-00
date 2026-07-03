/// camera-node 简易日志 —— 走 CAMERA_LOG_LEVEL env 控级别
///
/// 用法（与 Rust 端 log crate 的 error!/warn!/info!/debug!/trace! 风格一致）：
///     CAM_ERROR("open device failed: {}", errno);
///     CAM_INFO (">>> capture started");
///     CAM_DEBUG("{}x{} {} | {} fps | {} KB/frame", w, h, fmt, fps, kb);
///
/// 级别从 `[logging] level` 读，配置默认值 info。init.sh 会把配置值 export 成
/// `CAMERA_LOG_LEVEL` 传给本节点（C++ 不能直接复用 dora-config Rust crate）。
///
/// 阈值（数字越大越详尽）：
///   0 error   1 warn   2 info   3 debug   4 trace

#pragma once

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace camlog {

enum Level : int {
    LVL_ERROR = 0,
    LVL_WARN  = 1,
    LVL_INFO  = 2,
    LVL_DEBUG = 3,
    LVL_TRACE = 4,
};

inline Level parse_level(const char* s) {
    if (!s || !*s) return LVL_INFO;
    if (std::strcmp(s, "error") == 0) return LVL_ERROR;
    if (std::strcmp(s, "warn")  == 0) return LVL_WARN;
    if (std::strcmp(s, "info")  == 0) return LVL_INFO;
    if (std::strcmp(s, "debug") == 0) return LVL_DEBUG;
    if (std::strcmp(s, "trace") == 0) return LVL_TRACE;
    return LVL_INFO;
}

inline Level current_level() {
    static const Level kLevel = parse_level(std::getenv("CAMERA_LOG_LEVEL"));
    return kLevel;
}

inline const char* level_name(Level l) {
    switch (l) {
        case LVL_ERROR: return "ERROR";
        case LVL_WARN:  return "WARN";
        case LVL_INFO:  return "INFO";
        case LVL_DEBUG: return "DEBUG";
        case LVL_TRACE: return "TRACE";
    }
    return "?";
}

inline void log_emit(Level lvl, const std::string& msg) {
    if (lvl > current_level()) return;
    std::cout << "[camera-cpp/" << level_name(lvl) << "] " << msg << std::endl;
}

}  // namespace camlog

// ── 便捷宏（接受 printf 风格格式串）──

#define CAM_LOG_AT(lvl, ...) do { \
    if ((lvl) <= camlog::current_level()) { \
        char _cam_buf[1024]; \
        std::snprintf(_cam_buf, sizeof(_cam_buf), __VA_ARGS__); \
        camlog::log_emit((lvl), _cam_buf); \
    } \
} while (0)

#define CAM_ERROR(...) CAM_LOG_AT(camlog::LVL_ERROR, __VA_ARGS__)
#define CAM_WARN(...)  CAM_LOG_AT(camlog::LVL_WARN,  __VA_ARGS__)
#define CAM_INFO(...)  CAM_LOG_AT(camlog::LVL_INFO,  __VA_ARGS__)
#define CAM_DEBUG(...) CAM_LOG_AT(camlog::LVL_DEBUG, __VA_ARGS__)
#define CAM_TRACE(...) CAM_LOG_AT(camlog::LVL_TRACE, __VA_ARGS__)
