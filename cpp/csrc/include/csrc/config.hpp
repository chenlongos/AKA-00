// csrc/config.hpp — 机器人配置（对应原 Python app/config.py 的 HardwareConfig）
//
// 读取 config.toml（TOML 子集，单 key 单 value），搜索顺序：
//   1. $AKA_HOME/etc/config.toml（生产部署）
//   2. 可执行文件所在目录的 ../etc/config.toml（bin/aka-capp → ../etc/config.toml）
//   3. CWD 下的 config.toml（开发）
// 都找不到则用默认值（motor/arm backend=dev，不控制硬件）。

#pragma once

#include <string>

namespace csrc {

struct CameraConfig {
    int width = 320;
    int height = 240;
    int fps = 24;
    int jpeg_quality = 30;
};

struct MotorConfig {
    std::string backend = "dev";   // "dev" (mock) | "tt_pid"
    std::string port = "/dev/ttyS1";
    int baudrate = 115200;
    int ppr = 4680;
};

struct ArmConfig {
    std::string backend = "dev";   // "dev" | "zp10s" | "sts3215"
    std::string port = "/dev/ttyS2";
    int baudrate = 115200;
};

struct WebConfig {
    int port = 80;
    int https_port = 5443;
};

struct OtaConfig {
    std::string check_url = "https://api.chenlongrobot.com/api/user/robot-versions/featured";
};

struct ChassisConfig {
    double wheel_diameter_mm = 62.0;
    int gear_ratio = 90;
};

struct LoggingConfig {
    std::string level = "info";
};

struct Config {
    CameraConfig camera;
    MotorConfig motor;
    ArmConfig arm;
    WebConfig web;
    OtaConfig ota;
    ChassisConfig chassis;
    LoggingConfig logging;

    // 云端 URL（app/config.py HardwareConfig 对齐）
    std::string demo_server_url = "http://124.222.162.228:8888";
    std::string status_report_url = "https://api.chenlongrobot.com/api/robot-actions";

    // 距离标定: D = m / P + c
    double calib_m = 2671.82;
    double calib_c = -2.82;

    /// 加载配置（见文件头搜索顺序）。失败时 warn + 返回默认值。
    static Config load();

    /// 返回实际使用的 config.toml 路径（可能为空 = 使用默认值）。
    static std::string find_config_path();
};

}  // namespace csrc
