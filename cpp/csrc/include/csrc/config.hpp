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
    int https_port = 5443;        // 0 = HTTPS disabled
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
/// 板上实测：屏是 SPI 接口，写屏带宽是硬瓶颈——出厂 4MHz 时 ~420KB/s（约 2fps），
/// 该板稳定上限 20MHz（~2MB/s，需改设备树 spi-max-frequency）。故默认半屏
/// （scale=2 → 160x240，75KB/帧）以吃满摄像头帧率。显示线程与浏览器流共享同一份
/// 解码结果（Camera::latest_rgb 缓存），开屏不会拖慢浏览器看摄像头画面。
struct DisplayConfig {
    bool enabled = true;     // 是否启用屏显示（无 /dev/fb0 时自动跳过）
    /// 屏显示跟随摄像头开关（默认 true）：
    ///   摄像头关（默认开机态）→ 屏保持黑，不出图；
    ///   打开摄像头（前端开关 / POST /api/camera/open）→ 屏开始显示；
    ///   关闭摄像头（POST /api/camera/close）→ 屏清屏熄灭。
    /// false = 开机就常显（等价于旧行为，会顺带打开摄像头）。
    bool follow_camera = true;
    int scale = 2;           // 显示区域 = 屏幕 1/scale（2 → 160x240 居中）；1 = 全屏
    int orient = 3;          // 0无 1水平翻 2垂直翻 3=180°（本板实测 3 为正）
    int fps = 15;            // 显示帧率上限（建议与 camera.fps 一致）
    /// 浏览器有人在看 MJPEG 时的显示帧率（默认 5；0 = 暂停显示）。
    /// 单核 SoC 上"显示 + 浏览器流"会 CPU 饱和：显示每帧的 RGB565 转换 + SPI
    /// 写屏约 20ms（15fps 下 ≈30% 单核），会显著拉低浏览器取流的帧率/延迟。
    /// 有人看流时把屏幕降到该帧率，浏览器优先；没人看时恢复 fps。
    int fps_streaming = 5;
    int noise = 1;           // 脏行容差：忽略每通道 N 个 LSB（0=精确，越大越宽容）
    int decode_max_w = 320;  // 解码降采样上限宽（与 camera.stream_width 一致可命中共享缓存）
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
