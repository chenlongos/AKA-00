// capp/context.hpp — 应用共享状态（对应 app/services/* 的全局单例）
//
// AppContext 持有全部服务状态：
//   - 硬件: MotorPair / Gripper / Camera（csrc）
//   - 状态采集: StateCollector（csrc 单例）
//   - 控制服务: 定时停线程 / 夹爪锁
//   - demo: 就是"拿某个模型跑一遍 Lua 流程"（薄封装，见 routes.cpp）
//   - 脚本: Lua 流程宿主（scripts/*.lua）
//   - ota: 升级任务
//   - 云端上报: 命令日志
//
// 服务方法实现见 services.cpp；路由处理见 routes.cpp。

#pragma once

#include <atomic>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "csrc/angle_config.hpp"
#include "csrc/camera.hpp"
#include "csrc/config.hpp"
#include "csrc/gripper.hpp"
#include "csrc/json.hpp"
#include "csrc/motor_pair.hpp"
#include "csrc/screen_display.hpp"
#include "csrc/state.hpp"
#include "csrc/yolo_detector.hpp"

namespace capp {

struct AppContext {
    csrc::Config config;
    std::unique_ptr<csrc::MotorPair> motor_pair;
    /// 若 motor_pair 是自动重连代理（backend=tt_pid），指向它（暴露连接状态/触发重连）
    csrc::AutoReconnectMotorPair* motor_link = nullptr;
    std::unique_ptr<csrc::Gripper> gripper;
    csrc::StateCollector& collector = csrc::StateCollector::get_instance();
    csrc::Camera& camera = csrc::Camera::get_instance();
    /// 板载 SPI 屏显示（摄像头画面 → /dev/fb0）
    csrc::ScreenDisplay display;

    // 单帧推理（GET /api/detect）：懒加载的模型 + 一把锁。
    // 同步跑（每请求一次推理），锁把"换模型 + 推理"整段罩住 —— TPU 是单实例、
    // YoloDetector 非线程安全；脚本并发由 script_running 串行（同一时刻只有一个流程）。
    // Lua 流程脚本（scripts/*.lua）—— 状态由工作线程写、接口读，都用 script_mu 保护；
    // script_abort 是给"立即停"用的（原子，免得停止请求要等锁）。
    std::mutex script_mu;
    std::thread* script_thread = nullptr;
    std::atomic<bool> script_abort{false};
    bool script_running = false;
    std::string script_state = "idle";   // idle|running|done|failed|aborted
    std::string script_message;
    std::string script_name;
    long long script_elapsed_ms = 0;
    long long script_calls = 0;                  // 原语调用计数（看脚本有没有在动）
    std::string script_action;                   // 最近一次动作
    std::vector<std::pair<std::string, std::string>> script_notes;   // 脚本 note() 发布的字段

    std::mutex detect_mu;
    std::string detect_model;                      // 当前已加载的模型名（空 = 没加载）
    // 加载时模型文件的 mtime / 大小。同名模型被重新下载覆盖后，下一个请求就换新的
    // —— 否则"覆盖"对已经加载着的模型是空的（要重启 capp 才生效）。
    long detect_mtime = 0;
    long long detect_size = 0;
    std::unique_ptr<csrc::YoloDetector> detector;  // 首次请求时才加载

    bool camera_on = false;

    /// 优雅关闭标记（SIGTERM/SIGINT → main 设置 → accept 循环退出）
    std::atomic<bool> shutdown{false};

    // 控制服务
    std::mutex timer_mu;
    std::thread* timer_thread = nullptr;
    std::atomic<bool> timer_cancel{false};
    /// 控制指令代际号（timer_mu 保护）：每条新指令推进；同步等待方据此判断
    /// "自己发起的运动是否已被后续指令取代"（被取代则不再自动停车）
    int motion_seq = 0;
    std::mutex arm_mu;   // grab/release 串行

    // demo 模型下载进度: task_id → Json{progress, status, error}
    std::mutex dl_mu;
    std::map<std::string, csrc::Json> downloads;

    // ota 任务: task_id → Json{progress, status, message}
    std::mutex ota_mu;
    std::map<std::string, csrc::Json> ota_tasks;

    // 云端上报命令日志
    std::mutex cmdlog_mu;
    std::vector<csrc::Json> command_log;

    // 路径
    std::string app_dir;      // 项目根（含 static/、demo/、arm_angles.json）
    std::string static_dir;   // 前端静态目录
    std::string version;      // VERSION 文件内容（OTA /version 用）

    /// 记录控制命令（status_reporter 上报用）
    void log_command(csrc::Json cmd) {
        std::lock_guard<std::mutex> lk(cmdlog_mu);
        csrc::Json entry;
        entry["ts"] = csrc::Json((int64_t)(::time(nullptr)));
        for (auto& kv : cmd.object()) entry[kv.first] = kv.second;
        command_log.push_back(entry);
        if (command_log.size() > 20) command_log.erase(command_log.begin());
    }
};

// ── 控制服务（对应 app/services/control_service.py）──

/// 初始化硬件服务（启动 StateCollector）。返回 false 表示全部 mock。
bool init_services(AppContext& ctx);

/// 动作控制: action = up/down/left/right/stop/grab/release
/// wait_done=true 且带时长时：阻塞到运动执行完（自动停车）后才返回确认 ACK。
csrc::Json execute_action(AppContext& ctx, const std::string& action, int speed,
                          double milliseconds, bool wait_done = false);
/// 直接设置电机速度（可选持续时间秒）。wait_done=true 时阻塞到时长结束停车后才返回。
csrc::Json run_motor(AppContext& ctx, int left, int right, double duration,
                     bool wait_done = false);
/// 闭环距离/转向（ESP32 固件内部执行）
csrc::Json move_distance(AppContext& ctx, const std::string& direction, double value, int speed);
/// 发送原始命令到夹爪串口
csrc::Json send_raw_command(AppContext& ctx, const std::string& cmd);
/// 更新机械臂角度配置
csrc::Json update_arm_angles(AppContext& ctx, const std::string& driver, const csrc::Json& angles);
/// 预览机械臂角度（立即执行）
csrc::Json preview_arm_angle(AppContext& ctx, const std::string& driver, const std::string& key, int angle);
/// 重新初始化底盘（强制断开并立即重连真实底盘）
csrc::Json reinitialize_motor_pair(AppContext& ctx);
/// 底盘连接状态对象（backend/enabled/connected/state/attempts/error）
csrc::Json motor_status_json(AppContext& ctx);

// ── 底盘/机械臂的底层原语（服务层内部用；脚本宿主 capp/script.cpp 也复用）──

/// 取一帧跑一次推理 → 框列表（原图像素坐标、已 NMS）。`/api/detect` 与脚本原语共用。
bool detect_boxes(AppContext& ctx, const std::string& model_name, const csrc::DecodeOptions& opt,
                  std::vector<csrc::Detection>& out, int& frame_w, std::string& err);

/// 取消挂起的"定时停"线程
void cancel_pending_stop(AppContext& ctx);
/// 推进控制指令代际号（每条新指令都要推；同步等待方据此判断自己是否已被取代）
int64_t bump_motion_seq(AppContext& ctx);
/// 读当前代际号
int64_t motion_seq_now(AppContext& ctx);
/// 跑 duration 秒后自动停车（同步阻塞）。0=正常 1=被后续指令取代 2=应用退出
int wait_timed_done(AppContext& ctx, int64_t seq, double duration_sec);
/// 底盘动作：up/down/left/right/stop（速度百分比，调用方负责 clamp）
bool apply_base_action(AppContext& ctx, const std::string& action, int speed);
/// 机械臂动作：grab/release（内部持 arm_mu，异步执行）
bool apply_arm_action(AppContext& ctx, const std::string& action);
/// 底盘连接状态 JSON（含 connected 字段）
csrc::Json motor_status_json(AppContext& ctx);

// ── 摄像头服务（对应 app/services/camera_service.py）──

/// 确保摄像头已打开
bool ensure_camera(AppContext& ctx);
/// 关闭摄像头
void close_camera(AppContext& ctx);
/// 当前帧 → JPEG 字节（原生 MJPEG 直通；YUYV 先转 RGB 再编码）
bool current_jpeg(AppContext& ctx, int quality, std::vector<uint8_t>& out);
/// 流帧 → JPEG 字节：按 config.camera.stream_* 缩放重编码（=0 时等价直通）。
/// 返回 false = 帧不可用。
bool build_stream_jpeg(AppContext& ctx, const csrc::Camera::Frame& f, std::vector<uint8_t>& out);
/// 流帧 → JPEG 字节（用共享解码结果版本）：屏幕显示与浏览器流共用一次解码。
bool build_stream_jpeg_rgb(AppContext& ctx, const csrc::Camera::RgbFrame& rgb,
                           std::vector<uint8_t>& out);

// ── 单帧推理服务（GET /api/detect）──

/// 模型名是否合法：只允许 [A-Za-z0-9_.-]。
/// 必须校验 —— 名字会拼进文件路径，否则 `?model=../../etc/passwd` 就是任意文件读取。
bool valid_model_name(const std::string& name);

/// 把上传上来的模型内容写进 `$AKA_HOME/models/<name>.cvimodel`（**同名覆盖**）。
/// 给"平台推模型"用：content 就是请求体。落地前校验（`CviModel` 魔数 + 大小上限）
/// 并原子换入，坏包不会覆盖掉正在用的模型。返回 `{ok, name, path, size}` 或 `{ok:false, error}`。
csrc::Json save_model_upload(AppContext& ctx, const std::string& name, const std::string& content);


// ── Lua 流程脚本（scripts/*.lua，实现在 capp/script.cpp）──
//
// 把"看→对准→靠近→抓"这类**要反复调参的流程**从 C++ 搬到脚本里：改一行存盘重跑，
// 不用交叉编译 + 部署 + 重启。脚本只拿得到有上限的原语；超时/限速/被抢占地接管/
// 底盘掉线这些**安全兜底全在宿主**（见 script.cpp 的注释与文档）。

/// 跑一个脚本（异步；同一时刻只允许一个）。params 会以 Lua table 的形式给脚本读。
/// 脚本从 `$AKA_HOME/scripts/<name>.lua` 读；名字只允许 [A-Za-z0-9_.-]。
/// max_seconds 是宿主强制的总时长上限（clamp 到 5..300），到点宿主会打断脚本并停车。
csrc::Json script_run(AppContext& ctx, const std::string& name, const csrc::Json& params,
                      int max_seconds);
/// 停止当前脚本：置中止标志并立刻刹车（不等脚本配合）。
csrc::Json script_stop(AppContext& ctx);
/// 当前状态：state / script / message / elapsed_ms / calls / action / notes
csrc::Json script_status(AppContext& ctx);

/// 取当前摄像头帧跑一次推理。
/// 成功：{"ok":true,"count":N,"boxes":[{"x1","y1","x2","y2"}...]}（原图像素坐标）
/// 失败：{"ok":false,"error":"..."}（HTTP 码由路由决定）
csrc::Json detect_once(AppContext& ctx, const std::string& model_name);

// ── 板载屏显示服务 ──

/// 屏显示开关（运行时）：关掉会立刻停屏，打开会在摄像头已开时立刻起屏。
/// 只改内存里的 config.display.enabled —— **不写回 config.toml**：
/// 参数文件是用户现场改的，重启后回到文件里的值（避免"重启后屏莫名其妙黑了"）。
/// 顺带说明为什么需要它：全屏写屏很吃那颗单核 CPU（实测 /api/detect 从 120ms 涨到
/// 340ms），要在追物/检测时让出 CPU 就把它关掉。
csrc::Json display_config(AppContext& ctx);
csrc::Json set_display_enabled(AppContext& ctx, bool enabled);

/// 启动屏显示（按 config.display；无 /dev/fb0 时返回 false 但不影响其它服务）
bool ensure_display(AppContext& ctx);
/// 停止屏显示并释放 framebuffer
void close_display(AppContext& ctx);
/// 屏显示状态（JSON：running/available/fps/frames/区域尺寸等）

// ── 状态上报（对应 app/services/status_reporter.py）──

/// 启动云端状态上报线程（URL 为空则不启动）
void start_status_reporter(AppContext& ctx);
/// 立即上报一次（boot/heartbeat）
void report_status(AppContext& ctx, const std::string& action);

}  // namespace capp
