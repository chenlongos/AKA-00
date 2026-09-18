// csrc/config.hpp — 机器人配置（对应原 Python app/config.py 的 HardwareConfig）
//
// 读取 config.toml（TOML 子集，单 key 单 value），搜索顺序：
//   1. $AKA_HOME/etc/config.toml（旧布局；当前打包布局是 $AKA_HOME/config.toml，见 1b）
//   2. 可执行文件所在目录的 ../etc/config.toml（bin/aka-capp → ../etc/config.toml）
//   3. CWD 下的 config.toml（开发）
// 都找不到则用默认值（motor 默认 tt_pid —— mock 已删除，连不上会明确报错；
// arm 默认 zp10s）。

#pragma once

#include <string>

namespace csrc {

struct CameraConfig {
    int width = 640;    // 摄像头采集宽度：须为原生可出流档（Hy-UXGA B5M2 实测 640x360；320x240 是假档勿用）
    int height = 360;
    int fps = 15;
    int jpeg_quality = 30;
    // ── 浏览器取流方式（CPU ↔ 带宽）──
    // false（默认）：直通摄像头原帧 —— 服务端零解码零编码，单核 SoC 上
    //   "网页看摄像头不卡"的关键；代价是带宽大（可用 jpeg_quality 调小）。
    // true：服务端缩放到 stream_width/height 并重编码下发 —— 省带宽但每帧多
    //   ~15ms 编码 CPU（单核上会拖慢取流）。
    bool stream_scale = false;
    int stream_width = 320;
    int stream_height = 180;
    int stream_quality = 60;
    // 固定曝光/AWB/增益（默认关）：廉价 UVC 的自动曝光/AWB 周期性抖动会让整幅
    // 画面每帧一起变，屏显示的脏行检测失效（写屏流量上升、帧率下降）。
    // 开启后画面亮度恒定（暗光下会偏暗），与 demo 的 DEMO_EXP_FIX=1 等价。
    bool exp_fix = false;
};

struct MotorConfig {
    // mock 已删除（2026-09-18）：只认 "tt_pid"；其它值会被明确报错而不是静默不驱动。
    // 默认值也从 "dev" 改成 "tt_pid" —— 配置缺失时宁可去连真底盘（连不上会报错），
    // 也不要退成一个"看着在跑、其实不动"的状态。
    std::string backend = "tt_pid";
    std::string port = "/dev/ttyS1";
    int baudrate = 115200;
    int ppr = 4680;
};

struct ArmConfig {
    // mock 已删除（2026-09-18）：只认 "zp10s" | "sts3215"；默认值跟着本仓库的
    // config.toml 走（板和包的默认硬件都是 zp10s）。其它值不复位成 mock。
    std::string backend = "zp10s";
    std::string port = "/dev/ttyS2";
    int baudrate = 115200;
};

struct WebConfig {
    int port = 80;
    int https_port = 443;         // 0 = HTTPS disabled（443 让 https://<ip>/ 直接可用、wss 同端口）
    std::string https_cert = "cert.pem";   // 相对 $AKA_HOME 或绝对路径
    std::string https_key  = "key.pem";
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

/// 板载 SPI 屏实时显示（/dev/fb0，ST7796S 320x480 RGB565）
///
/// 板载屏显示（摄像头画面 → /dev/fb0）。默认值 = cpp/config.toml 里发出去的值；
/// 每个字段的取舍理由写在 config.toml 的注释与 README 的"板载屏显示"章节，这里只留摘要。
struct DisplayConfig {
    bool enabled = true;        // 关掉则整个显示栈不启动（无 /dev/fb0 时也自动跳过）
    int scale = 1;              // 显示区域 = 屏幕 1/scale（1=全屏 320x480；2=半屏）
    int orient = 3;             // 0无 1水平翻 2垂直翻 3=180°（本板实测 3 为正）
    int fps = 8;                // 显示帧率上限（全屏一帧 307KB，SPI 上限约 8fps）
    int fps_streaming = 3;      // 浏览器在看流时的显示帧率（0=暂停；浏览器优先）
    int decode_max_w = 640;     // 解码降采样上限宽（全屏 640 清晰；半屏可 320 省 CPU）
    std::string standby_image = "start_img.jpg";  // 熄屏待机图（空=黑屏），铺满整屏
};

struct Config {
    CameraConfig camera;
    MotorConfig motor;
    ArmConfig arm;
    WebConfig web;
    OtaConfig ota;
    ChassisConfig chassis;
    LoggingConfig logging;
    DisplayConfig display;

    // 云端 URL（app/config.py HardwareConfig 对齐）
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
