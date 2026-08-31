// csrc/log.hpp — 轻量日志（stderr），风格与 screen-node/log.h 一致
//
// 级别: error / warn / info / debug（默认 info）
// 控制: 环境变量 CSRC_LOG_LEVEL 或宏 CSRC_LOG_LEVEL_DEFAULT
// 统一走 stderr，方便上层守护脚本捕获。

#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace csrc {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

inline LogLevel log_level() {
    static LogLevel level = [] {
        const char* env = getenv("CSRC_LOG_LEVEL");
        if (!env) return LogLevel::Info;
        std::string s = env;
        if (s == "error") return LogLevel::Error;
        if (s == "warn") return LogLevel::Warn;
        if (s == "debug" || s == "trace") return LogLevel::Debug;
        return LogLevel::Info;
    }();
    return level;
}

inline void log_write(LogLevel lv, const char* tag, const char* fmt, ...) {
    if ((int)lv > (int)log_level()) return;
    char ts[32];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
    fprintf(stderr, "[%s][%s] ", ts, tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

}  // namespace csrc

#define CAM_ERROR(fmt, ...) ::csrc::log_write(::csrc::LogLevel::Error, "ERR", fmt, ##__VA_ARGS__)
#define CAM_WARN(fmt, ...)  ::csrc::log_write(::csrc::LogLevel::Warn,  "WRN", fmt, ##__VA_ARGS__)
#define CAM_INFO(fmt, ...)  ::csrc::log_write(::csrc::LogLevel::Info,  "INF", fmt, ##__VA_ARGS__)
#define CAM_DEBUG(fmt, ...) ::csrc::log_write(::csrc::LogLevel::Debug, "DBG", fmt, ##__VA_ARGS__)
