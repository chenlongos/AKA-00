// capp/context.hpp — 应用共享状态（对应 app/services/* 的全局单例）
//
// AppContext 持有全部服务状态：
//   - 硬件: MotorPair / Gripper / Camera（csrc）
//   - 状态采集: StateCollector（csrc 单例）
//   - 控制服务: 定时停线程 / 夹爪锁
//   - demo: 卡片 = 动作 × 模型（跑 demo/<动作>.lua，模型由宿主注入 params.model；见 routes.cpp）
//   - 脚本: Lua 流程宿主（$AKA_HOME/demo/*.lua）
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
#include <pthread.h>   // script_tid 是 pthread_t：自己 include，别指望 <thread> 间接带进来
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
    // ── Lua 动作脚本（demo/*.lua）── 状态由工作线程写、接口读，都用 script_mu 保护；
    // script_abort 是给"立即停"用的（原子，免得停止请求要等锁）。
    std::mutex script_mu;
    /// 脚本工作线程（pthread 而不是 std::thread：**要显式指定栈大小**）。
    /// musl 的 std::thread 默认栈只有 128KB（glibc 是 8MB），而脚本线程的调用链是
    /// Lua VM → 原语 → 取帧 → libjpeg 解码（jpeg_decompress_struct 本身就十几 KB），
    /// 栈溢出会踩到相邻内存 —— 实测表现为在 jpeg_idct_* 里收到 badaddr≈0x46 的段错误。
    pthread_t script_tid{};
    bool script_tid_valid = false;
    std::atomic<bool> script_abort{false};
    bool script_running = false;
    std::string script_state = "idle";   // idle|running|done|failed|aborted
    std::string script_message;
    std::string script_name;    // 动作脚本名（demo/<动作>.lua）
    std::string script_model;   // 这次跑的是哪个模型（params.model；脚本路径之外还要能答"在追什么"）
    std::string script_card;    // 哪张卡片发起的（直接调 action+model 跑时为空）
    bool script_repeat = false; // 循环执行（mode=loop）：跑完一轮接着下一轮，直到被停
    int script_round = 0;       // 已跑到第几轮（once 恒为 1）
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

/// 初始化硬件服务（启动 StateCollector）。返回值恒为 true（底盘已无 mock，见 motor_pair.hpp）。
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
/// 机械臂动作的结果：不是机械臂动作 / 已受理（后台执行中）/ 夹爪正忙（这次跳过，没排队）
enum class ArmResult { NotArm, Accepted, Busy };
/// 机械臂动作：grab/release。**不排队** —— 正忙时返回 Busy，由调用方如实回报给用户。
ArmResult apply_arm_action(AppContext& ctx, const std::string& action);
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

// ── demo 资源（全部在 `$AKA_HOME/demo/` 下，见 cpp/README.md 的部署布局）──
//
//   demo/<动作>.lua            **动作脚本**（预定义、与模型无关，模型从 params().model 读）
//   demo/models/*.cvimodel     模型库
//   demo/configs/<卡片名>.json  **卡片定义**：{"action":..,"model":..,+ 四个参数}
//
// 一张 demo 卡片 = 动作 × 模型（用户自己在界面上建，名字自由）；跑的时候宿主读卡片拿到
// 动作和模型，跑 demo/<动作>.lua 并把 params.model 注入成卡片里的模型。
// 注意卡片名**只当文件名用**（可能是中文），别拿它去拼模型路径 —— 这是最容易犯、
// 报错又最误导的一处（会变成"注册模型失败：…/demo/models/追网球接近.cvimodel"）。

/// `$AKA_HOME/demo/models`
std::string model_dir(AppContext& ctx);
/// `$AKA_HOME/demo/models/<name>.cvimodel`
std::string model_path(AppContext& ctx, const std::string& name);

/// `$AKA_HOME/demo/<动作>.lua`（参数是**动作名**，不是卡片名、更不是模型名）
std::string action_script_path(AppContext& ctx, const std::string& name);
/// 这个动作脚本在不在（列表用它标 script 字段，跑之前也用它先挡一道）
bool action_script_exists(AppContext& ctx, const std::string& name);

/// 卡片名是否合法：只有文件系统层面的限制（允许中文），见实现里的说明
bool valid_card_name(const std::string& name);

/// 把上传上来的模型内容写进 `model_path()`（**同名覆盖**）。
/// 给"平台推模型"用：content 就是请求体。落地前校验（`CviModel` 魔数 + 大小上限）
/// 并原子换入，坏包不会覆盖掉正在用的模型。返回 `{ok, name, path, size}` 或 `{ok:false, error}`。
csrc::Json save_model_upload(AppContext& ctx, const std::string& name, const std::string& content);


// ── Lua 流程脚本（$AKA_HOME/demo/*.lua，实现在 capp/script.cpp）──
//
// 把"看→对准→靠近→抓"这类**要反复调参的流程**从 C++ 搬到脚本里：改一行存盘重跑，
// 不用交叉编译 + 部署 + 重启。脚本只拿得到有上限的原语；限速/被抢占的接管/
// 底盘掉线这些**安全兜底全在宿主**（见 script.cpp 的注释与文档）。

/// 跑一个动作脚本（异步；同一时刻只允许一个）。params 会以 Lua table 的形式给脚本读。
/// 脚本从 `$AKA_HOME/demo/<name>.lua` 读（name 是**动作名**）；名字只允许 [A-Za-z0-9_.-]。
/// 执行方式看 params.mode：`once`（默认，跑一遍就结束）/ `loop`（跑完接着跑，直到被停）。
/// **没有总时长上限** —— 停不停由 stop / 人的指令接管 / 服务退出决定。
csrc::Json script_run(AppContext& ctx, const std::string& name, const csrc::Json& params);
/// 停止当前脚本：置中止标志并立刻刹车（不等脚本配合）。
csrc::Json script_stop(AppContext& ctx);
/// 当前状态：state / script（动作名）/ mode / round / model / card / message /
/// calls / action / notes
csrc::Json script_status(AppContext& ctx);

/// 取当前摄像头帧跑一次推理。
/// 成功：{"ok":true,"count":N,"boxes":[{"x1","y1","x2","y2"}...]}（原图像素坐标）
/// 失败：{"ok":false,"error":"..."}（HTTP 码由路由决定）
/// 从 conf / iou 构造解码参数：0 表示"没给"→ 用默认（0.25 / 0.45），
/// 给了就夹到 0.01~0.99。/api/detect 的 ?conf=&iou= 与脚本的 detect(model, {conf=,iou=})
/// 都走这里，免得两处各写一套边界。
csrc::DecodeOptions decode_options(double conf, double iou);

/// 等机械臂动作（grab/release）做完 —— 它们是异步跑的，接口要等它才能报"完成"。
void wait_arm_done(AppContext& ctx);

/// 等脚本跑完（状态离开 running）。true = 已结束；false = 超时仍在跑。
/// 给"跑完再返回"的接口用（每个连接一个线程，阻塞不会卡住别的请求）。
bool wait_script_done(AppContext& ctx, double timeout_s);

/// 单帧推理（/api/detect 用）。opt 不给就用默认阈值（conf 0.25 / iou 0.45）。
csrc::Json detect_once(AppContext& ctx, const std::string& model_name,
                       const csrc::DecodeOptions& opt = {});

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
/// 在"摄像头已开"的前提下启动屏显示（不再回调 ensure_camera，避免递归）。
/// ensure_display 与 ensure_camera（摄像头一开就跟着起屏）都要用，所以是跨文件的。
bool start_display_locked_on_camera(AppContext& ctx);
/// 停止屏显示并释放 framebuffer
void close_display(AppContext& ctx);
/// 屏显示状态（JSON：running/available/fps/frames/区域尺寸等）

// ── 状态上报（对应 app/services/status_reporter.py）──

/// 读 $AKA_HOME/VERSION（"v1.2.3@1722169200" 或 "v1.2.3 1722169200"）→ 版本号 + 时间戳。
/// 状态上报与 OTA 比版本都用它（**只有这一份解析**）。
void read_version_file(AppContext& ctx, std::string& ver, int64_t& ts);

/// 启动云端状态上报线程（URL 为空则不启动）
void start_status_reporter(AppContext& ctx);
/// 立即上报一次（boot/heartbeat）
void report_status(AppContext& ctx, const std::string& action);

// ── WiFi STA 服务（wlan1 连目标路由器；实现在 services/wifi_service.cpp）──
//
// 除了 app/routes/wifi.py 那套 wpa_supplicant 自举，这里多做一件事：
// **把最后一次成功连接的 WiFi 记下来**，capp 下次启动时后台重放，用户不用每次开机
// 都在界面里重连。凭据落 /etc/aka-wifi.json（0600），只留最后一个。
//
// 为什么放 /etc 而不是 $AKA_HOME：OTA 的 --update 是整目录换包，$AKA_HOME 里不进
// KEEP_FILES 的东西会丢（KEEP_FILES 的 `-s` 判空还会静默扔掉 0 字节文件）；
// /etc 不在 swap_in 的范围内，升级天然保留，也和 AP 侧的 /etc/hostapd.conf 同处一地。

/// 凭据文件路径（默认 /etc/aka-wifi.json）。AKA_WIFI_CONF 可覆盖，供本机调试 ——
/// 免得在开发机上往真 /etc 写一个真的 wifi 密码文件。
std::string wifi_cred_path();

/// 存最后一次成功连接的 WiFi（ssid 为空 → 返回 false 且不写）
bool wifi_save_credential(const std::string& ssid, const std::string& password);

/// 读回凭据。文件不存在 / 0 字节 / 非法 JSON / 不是对象 / ssid 为空 → false（当"没存过"）
bool wifi_load_credential(std::string& ssid, std::string& password);

/// 确保 wlan1 的控制接口就绪（必要时拉起网卡并后台启动 wpa_supplicant）。
/// 注意：它**只看 socket 文件在不在** —— 返回 true 不代表 wpa_supplicant 真的活着
/// （进程崩了 socket 可能残留），静默路径上要自己用 `wpa_cli ping` 探活。
bool ensure_wpa_env();

/// 把一份凭据下发给 wpa_supplicant 并选中：
///   remove_network all → add_network → set_network <id> ssid <hex>
///   → psk "<pw>" | key_mgmt NONE → select_network <id>
/// **整段在互斥锁里**：调用方里有并发（http 是 thread-per-connection，用户点"连接"
/// 和启动重放会同时发命令），交叉执行的后果是网络表被搅成"ssid 是 A、psk 是 B"。
/// 返回 true 只代表**命令下发成功**，不代表连上了 —— 等待与判定由调用方自己做。
bool wifi_apply_network(const std::string& ssid, const std::string& password);

/// wlan1 的 wpa_state（"COMPLETED" / "SCANNING" / …），拿不到返回空串
std::string wifi_wpa_state();
/// wlan1 当前关联的 SSID，未关联返回空串
std::string wifi_current_ssid();

/// 启动后台"重放上次连接"线程（detach）。
/// 线程**不捕获 AppContext**（只读凭据文件 + 跑 wpa_cli），所以 main 的退出清理段
/// 不需要 join 它 —— 往这个线程里加 ctx 引用会引入悬垂，别加。
void start_wifi_autoconnect();

}  // namespace capp
