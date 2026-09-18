// capp/services.cpp — 服务层实现
//
// 对应 app/services/control_service.py + camera_service.py + status_reporter.py
// （行为/JSON 契约与原 Python app 保持一致）

#include "capp/context.hpp"

#include "capp/http_server.hpp"   // kMaxRequestBody（上传体上限，服务器与这里共用一个值）

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "csrc/http_client.hpp"
#include "csrc/log.hpp"
#include "csrc/system_utils.hpp"

namespace capp {

namespace {

// ── 定时停线程管理 ──

void schedule_stop(AppContext& ctx, double duration_sec) {
    std::lock_guard<std::mutex> lk(ctx.timer_mu);
    if (ctx.timer_thread) {
        ctx.timer_cancel = true;
        ctx.timer_thread->join();
        delete ctx.timer_thread;
    }
    ctx.timer_cancel = false;
    ctx.timer_thread = new std::thread([&ctx, duration_sec] {
        auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds((int64_t)(duration_sec * 1000.0));
        while (std::chrono::steady_clock::now() < until) {
            if (ctx.timer_cancel) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!ctx.timer_cancel) {
            ctx.motor_pair->sleep();
            std::lock_guard<std::mutex> lk2(ctx.timer_mu);
            ctx.timer_thread = nullptr;
        }
    });
}

// ── 同步"执行完再 ACK"辅助 ──

/// 等待底盘停稳（判定：曾经在动，且连续 stall_s 秒速度≈0）。
/// 返回 true=已停稳；false=超时。ever_moved 区分"走完停了"与"根本没动"。
bool wait_stationary(AppContext& ctx, double timeout_s, double stall_s, bool& ever_moved) {
    ever_moved = false;
    auto t0 = std::chrono::steady_clock::now();
    auto last_moving = std::chrono::steady_clock::now();
    while (true) {
        csrc::RobotStatus s = ctx.collector.get_status();
        bool moving = std::abs(s.left_speed) > 0.03 || std::abs(s.right_speed) > 0.03;
        auto now = std::chrono::steady_clock::now();
        if (moving) { ever_moved = true; last_moving = now; }
        double stopped_for = std::chrono::duration<double>(now - last_moving).count();
        double elapsed = std::chrono::duration<double>(now - t0).count();
        if (ever_moved && stopped_for >= stall_s) return true;
        // 从未观测到运动：为避免把"静止→即将起步"误判为完成，先观察 0.8s
        if (!ever_moved && elapsed >= 0.8) return true;
        if (elapsed >= timeout_s) return false;
        if (ctx.shutdown) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void do_grab(AppContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.arm_mu);
    ctx.gripper->close();
    ctx.collector.set_gripper_target(0);
}

void do_release(AppContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.arm_mu);
    ctx.gripper->open();
}

std::string read_version(AppContext& ctx) {
    std::ifstream f(ctx.app_dir + "/VERSION");
    if (!f) return "unknown";
    std::string raw;
    std::getline(f, raw);
    size_t at = raw.find('@');
    if (at != std::string::npos) return raw.substr(0, at);
    size_t sp = raw.find(' ');
    if (sp != std::string::npos) return raw.substr(0, sp);
    return raw.empty() ? "unknown" : raw;
}


}  // namespace

// ── 跨 TU 的控制原语 ──
// 定义必须在 capp 作用域（context.hpp 有声明；脚本宿主 capp/script.cpp 也要用），
// 不能放进上面的匿名 namespace，否则声明与定义分属两个名字，重载解析会歧义。
void cancel_pending_stop(AppContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.timer_mu);
    if (ctx.timer_thread) {
        ctx.timer_cancel = true;
        ctx.timer_thread->join();
        delete ctx.timer_thread;
        ctx.timer_thread = nullptr;
    }
}

int64_t motion_seq_now(AppContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.timer_mu);
    return ctx.motion_seq;
}

int64_t bump_motion_seq(AppContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.timer_mu);
    return ++ctx.motion_seq;
}

/// 等待 duration 秒后自动停车（同步阻塞）。返回:
///   0 = 正常：到点已 sleep() 停车
///   1 = 期间被后续指令取代（seq 变化，不自动停车，交由新指令接管）
///   2 = 应用退出
int wait_timed_done(AppContext& ctx, int64_t seq, double duration_sec) {
    auto until = std::chrono::steady_clock::now() +
                 std::chrono::milliseconds((int64_t)(duration_sec * 1000.0));
    while (std::chrono::steady_clock::now() < until) {
        if (ctx.shutdown) return 2;
        if (motion_seq_now(ctx) != seq) return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ctx.motor_pair->sleep();  // 到点滑行停车（与旧 schedule_stop 动作一致）
    return 0;
}

bool apply_base_action(AppContext& ctx, const std::string& action, int speed) {
    if (action == "up") {
        ctx.motor_pair->set_speed(speed, speed);
        ctx.collector.set_target_speed(speed, speed);
    } else if (action == "down") {
        ctx.motor_pair->set_speed(-speed, -speed);
        ctx.collector.set_target_speed(-speed, -speed);
    } else if (action == "left") {
        ctx.motor_pair->set_speed(-speed, speed);
        ctx.collector.set_target_speed(-speed, speed);
    } else if (action == "right") {
        ctx.motor_pair->set_speed(speed, -speed);
        ctx.collector.set_target_speed(speed, -speed);
    } else if (action == "stop") {
        ctx.motor_pair->brake();
        ctx.collector.set_target_speed(0, 0);
    } else {
        return false;
    }
    return true;
}

bool apply_arm_action(AppContext& ctx, const std::string& action) {
    if (action == "grab") {
        ctx.collector.set_gripper_target(1);
        ctx.collector.set_gripper_status("closed");
        std::thread([&ctx] { do_grab(ctx); }).detach();
        return true;
    }
    if (action == "release") {
        std::thread([&ctx] { do_release(ctx); }).detach();
        return true;
    }
    return false;
}

// ═══════════════════════ 初始化 ═══════════════════════

bool init_services(AppContext& ctx) {
    ctx.config = csrc::Config::load();

    // 底盘（先建：tt_pid 为自动重连代理，构造不阻塞、失败自动后台重试）
    ctx.motor_pair = csrc::create_motor_pair(ctx.config.motor.port, ctx.config.motor.backend,
                                             ctx.config.motor.baudrate, ctx.config.motor.ppr);
    ctx.motor_link = dynamic_cast<csrc::AutoReconnectMotorPair*>(ctx.motor_pair.get());
    ctx.collector.set_motor_pair(ctx.motor_pair.get());

    // 夹爪
    ctx.gripper = csrc::create_gripper(ctx.config.arm.backend, ctx.config.arm.port,
                                       ctx.config.arm.baudrate);

    // 状态采集
    ctx.collector.set_wheel_diameter_mm(ctx.config.chassis.wheel_diameter_mm);
    ctx.collector.set_gripper_status_provider([&ctx] {
        return std::string(csrc::gripper_status_str(ctx.gripper->get_status()));
    });
    ctx.collector.start();

    // 底盘和夹爪都没有 mock 了（2026-09-18 删除）：接不上就是明确报错 + 状态里体现，
    // 不再有"假装能动"的路径。返回值（"是否全是 mock"）恒为 true（没有 mock 可退）。
    CAM_INFO("[app] services ready (motor=%s arm=%s)",
             ctx.config.motor.backend.c_str(), ctx.config.arm.backend.c_str());
    return true;
}

// ═══════════════════════ 控制服务 ═══════════════════════

csrc::Json execute_action(AppContext& ctx, const std::string& action, int speed,
                          double milliseconds, bool wait_done) {
    cancel_pending_stop(ctx);
    int64_t seq = bump_motion_seq(ctx);
    CAM_INFO("[control] action=%s speed=%d ms=%.0f wait=%d", action.c_str(), speed,
             milliseconds, (int)wait_done);

    bool handled = apply_base_action(ctx, action, speed) || apply_arm_action(ctx, action);
    if (!handled) {
        csrc::Json err;
        err["status"] = "error";
        err["message"] = "unsupported action: " + action;
        return err;
    }

    bool timed_move = milliseconds > 0 &&
        (action == "up" || action == "down" || action == "left" || action == "right");
    if (timed_move && wait_done) {
        // 同步：阻塞到时长结束、自动停车后才返回确认 ACK
        int rc = wait_timed_done(ctx, seq, milliseconds / 1000.0);
        csrc::Json ok;
        ok["status"] = "success";
        ok["action"] = action;
        ok["completed"] = rc == 0;
        ok["duration_ms"] = csrc::Json((int64_t)milliseconds);
        ok["message"] = rc == 1 ? "superseded by newer command (no auto-stop)"
                                : action + " completed";
        return ok;
    }

    if (timed_move) {
        schedule_stop(ctx, milliseconds / 1000.0);
        csrc::Json ok;
        ok["status"] = "success";
        ok["message"] = action + " scheduled for " + std::to_string((long long)milliseconds) + "ms";
        return ok;
    }

    csrc::Json ok;
    ok["status"] = "success";
    ok["action"] = action;
    return ok;
}

csrc::Json run_motor(AppContext& ctx, int left, int right, double duration, bool wait_done) {
    cancel_pending_stop(ctx);
    int64_t seq = bump_motion_seq(ctx);
    ctx.motor_pair->set_speed(left, right);
    ctx.collector.set_target_speed(left, right);
    if (left == 0 && right == 0)
        CAM_INFO("[motor] stop cmd (L=R=0)");
    else
        CAM_DEBUG("[motor] run L=%d R=%d dur=%.2fs wait=%d", left, right, duration, (int)wait_done);
    if (duration > 0 && wait_done) {
        // 同步：阻塞到时长结束、自动停车后才返回确认 ACK
        int rc = wait_timed_done(ctx, seq, duration);
        csrc::Json ok;
        ok["status"] = "success";
        ok["left"] = csrc::Json((int64_t)left);
        ok["right"] = csrc::Json((int64_t)right);
        ok["duration"] = duration;
        ok["completed"] = rc == 0;
        ok["mode"] = "completed";
        return ok;
    }
    if (duration > 0) {
        schedule_stop(ctx, duration);
        csrc::Json ok;
        ok["status"] = "success";
        ok["left"] = csrc::Json((int64_t)left);
        ok["right"] = csrc::Json((int64_t)right);
        ok["duration"] = duration;
        ok["mode"] = "scheduled";
        return ok;
    }
    csrc::Json ok;
    ok["status"] = "success";
    ok["left"] = csrc::Json((int64_t)left);
    ok["right"] = csrc::Json((int64_t)right);
    return ok;
}

csrc::Json move_distance(AppContext& ctx, const std::string& direction, double value, int speed) {
    int sp = (int)(std::abs(speed));
    if (sp < 1) sp = 1;
    if (sp > 100) sp = 100;

    int d;
    if (direction == "forward") d = 0;
    else if (direction == "backward") d = 1;
    else if (direction == "left") d = 2;
    else if (direction == "right") d = 3;
    else {
        csrc::Json err;
        err["status"] = "error";
        err["message"] = "unknown direction: " + direction;
        return err;
    }

    int32_t target;
    std::string unit;
    if (d == 0 || d == 1) {
        target = (int32_t)std::llround(value);       // 直行：mm
        unit = "mm";
    } else {
        target = (int32_t)std::llround(value * 10);  // 转向：0.1°
        unit = "deg";
    }
    if (target <= 0) {
        csrc::Json err;
        err["status"] = "error";
        err["message"] = "target must be positive";
        return err;
    }

    bump_motion_seq(ctx);  // 取代任何进行中的定时运动
    auto* mp = ctx.motor_pair.get();
    int base = mp->move_state();  // 发送前状态（可能是上次闭环残留的 done=2）
    CAM_INFO("[control] move_distance dir=%s value=%.0f(%s) speed=%d target=%d base_state=%d",
             direction.c_str(), value, unit.c_str(), sp, (int)target, base);
    mp->move_distance((uint8_t)d, (uint8_t)sp, target);

    // 同步：等 ESP32 闭环精确回报。ESP32 把"运行中/结果"随 10Hz STATUS 回包附带
    // （主机 get_speeds 顺带解析成 move_state），这里只读内存标志，零新增串口流量。
    // 距离/转角大时该请求会挂几秒~几十秒，属预期（客户端勿设短超时）。
    auto t0 = std::chrono::steady_clock::now();
    const double kTimeoutS = 30.0;
    const double kFastGraceS = 0.8;  // 错过 running 帧时允许的宽限
    bool saw_running = false;
    int outcome = 0;  // 2=完成 3=中止 0=超时 -1=退出; <0 兜底见下
    double elapsed_ms = 0;
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        elapsed_ms = std::chrono::duration<double, std::milli>(now - t0).count();
        if (ctx.shutdown) { outcome = -1; break; }

        int st = mp->move_state();
        if (st < 0) {
            // mock 或无状态源（含等待中链路掉线切回 mock）：无法精确判定，
            // 退化为短等停稳直接返回（设备侧结果以 ESP32 自检为准）
            bool moved = false;
            if (base < 0) {
                wait_stationary(ctx, 2.0, 0.4, moved);
                outcome = 2;
            } else {
                outcome = 3;  // 曾经是真链路，中途失去状态源 → 按中止处理
            }
            break;
        }
        if (st == 1) saw_running = true;
        if (st == 2 || st == 3) {
            // 确认是"本次"的结果：观测到运行中、状态相对发送前有变化、或宽限已过
            if (saw_running || st != base || elapsed_ms >= kFastGraceS * 1000.0) {
                outcome = st;
                break;
            }
        }
        if (elapsed_ms >= kTimeoutS * 1000.0) { outcome = 0; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    csrc::Json ok;
    if (outcome == 2 || outcome == -1) {
        ok["status"] = "completed";
    } else if (outcome == 3) {
        ok["status"] = "aborted";
    } else {
        ok["status"] = "timeout";
    }
    ok["mode"] = "esp32";
    ok["target"] = value;
    ok["unit"] = unit;
    ok["moved"] = saw_running;
    ok["state"] = csrc::Json((int64_t)outcome);
    ok["elapsed_ms"] = csrc::Json((int64_t)elapsed_ms);
    return ok;
}

csrc::Json send_raw_command(AppContext& ctx, const std::string& cmd) {
    if (!cmd.empty()) ctx.gripper->send_raw_cmd(cmd);
    csrc::Json ok;
    ok["status"] = "success";
    ok["cmd"] = cmd;
    return ok;
}

csrc::Json update_arm_angles(AppContext& ctx, const std::string& driver, const csrc::Json& angles) {
    if (driver != ctx.config.arm.backend) {
        csrc::Json err;
        err["error"] = "driver mismatch: expected " + ctx.config.arm.backend + ", got " + driver;
        return err;
    }
    ctx.gripper->update_angles(angles);
    csrc::Json ok;
    ok["status"] = "success";
    ok["driver"] = driver;
    ok["angles"] = angles;
    return ok;
}

csrc::Json preview_arm_angle(AppContext& ctx, const std::string& driver, const std::string& key, int angle) {
    if (driver != ctx.config.arm.backend) {
        csrc::Json err;
        err["error"] = "driver mismatch: expected " + ctx.config.arm.backend + ", got " + driver;
        return err;
    }
    ctx.gripper->preview_angle(key, angle);
    csrc::Json ok;
    ok["status"] = "success";
    ok["driver"] = driver;
    ok["key"] = key;
    ok["angle"] = csrc::Json((int64_t)angle);
    return ok;
}

csrc::Json reinitialize_motor_pair(AppContext& ctx) {
    bool ok = false;
    if (ctx.motor_link) {
        ok = ctx.motor_link->reinitialize();  // 断开 + 立即完整重连（含 INIT/CONFIG）
    } else {
        ok = ctx.motor_pair->reinitialize();
    }
    csrc::Json j;
    j["status"] = "success";
    j["reinitialize"] = ok;
    j["motor"] = motor_status_json(ctx);
    return j;
}

csrc::Json motor_status_json(AppContext& ctx) {
    csrc::Json j;
    j["backend"] = ctx.config.motor.backend;
    bool connected = false;
    std::string state = "disabled";
    int attempts = 0;
    std::string error;
    // enabled 以**底盘自己的状态**为准，不再用 `backend != "dev"` 推：
    // 界面只在 enabled && !connected 时才提示"底盘未连接"，而配置一旦写成 dev
    // （或配置为空让默认值生效），按老算法 enabled=false → 车不动却一声不吭（踩过）。
    // mock 已删除，这个值现在恒为 true：要么连上，要么明确报未连接。
    bool enabled = true;
    if (ctx.motor_link) {
        csrc::MotorLinkStatus st = ctx.motor_link->link_status();
        enabled = st.enabled;
        connected = st.connected;
        state = st.state;
        attempts = st.attempts;
        error = st.error;
    }
    j["enabled"] = enabled;
    j["connected"] = connected;
    j["state"] = state;
    j["attempts"] = csrc::Json((int64_t)attempts);
    j["error"] = error;
    return j;
}

// ═══════════════════════ 摄像头服务 ═══════════════════════

// 内部：在"摄像头已开"的前提下启动屏显示（不再回调 ensure_camera，避免递归）
static bool start_display_locked_on_camera(AppContext& ctx) {
    csrc::DisplayConfig dc = ctx.config.display;
    if (dc.decode_max_w <= 0) dc.decode_max_w = ctx.config.camera.stream_width;
    if (!ctx.display.start(dc)) {
        CAM_INFO("[display] screen disabled (no framebuffer)");
        return false;
    }
    return true;
}

bool ensure_camera(AppContext& ctx) {
    if (ctx.camera_on) return true;
    // 固定曝光（可选，须在 open 前设置；对应 demo 的 DEMO_EXP_FIX=1）
    ctx.camera.set_fixed_exposure(ctx.config.camera.exp_fix);
    bool ok = ctx.camera.open(ctx.config.camera.width, ctx.config.camera.height,
                              ctx.config.camera.fps);
    ctx.camera_on = ok;
    // 屏显示跟随摄像头（固定行为）：摄像头一开，屏就出图
    if (ok && ctx.config.display.enabled) {
        if (!ctx.display.running()) {
            CAM_INFO("[display] 摄像头已开 → 屏开始显示");
            start_display_locked_on_camera(ctx);
        }
    }
    return ok;
}

void close_camera(AppContext& ctx) {
    // 先停显示（stop() 会清屏熄屏），再关摄像头
    close_display(ctx);
    ctx.camera.close();
    ctx.camera_on = false;
}

bool current_jpeg(AppContext& ctx, int quality, std::vector<uint8_t>& out) {
    csrc::Camera::Frame f;
    if (!ctx.camera.read_latest(f) || f.data.empty()) return false;
    if (csrc::Camera::is_jpeg(f.data.data(), f.data.size())) {
        out = std::move(f.data);
        return true;
    }
    // YUYV → RGB → JPEG
    if (f.format == 0 || f.w <= 0 || f.h <= 0) return false;
    std::vector<uint8_t> rgb((size_t)f.w * f.h * 3);
    csrc::Camera::yuyv_to_rgb(f.data.data(), f.w, f.h, rgb.data());
    return csrc::Camera::rgb_to_jpeg(rgb.data(), f.w, f.h, quality, out);
}

// 解码 → 等比缩放（黑边补齐到 stream_* 尺寸）→ 重编码 JPEG。
// 返回 false 表示该帧无法转出 JPEG（坏帧/未知格式），调用方应跳过而不是断开。
//
// 关键：解码走 Camera::latest_rgb（**共享缓存**）——屏幕显示线程与浏览器流共用
// 同一帧的解码结果，同一帧只解码一次。所以"开屏"不会拖慢浏览器：浏览器反而省掉
// 了自己那次整帧解码（640x360 MJPEG → 320 宽，C906 上约 10ms/帧）。
bool build_stream_jpeg(AppContext& ctx, const csrc::Camera::Frame& f,
                       std::vector<uint8_t>& out) {
    if (f.data.empty()) return false;
    csrc::Camera::RgbFrame rgb;
    if (!ctx.camera.latest_rgb(ctx.config.camera.stream_width, rgb) || rgb.data.empty())
        return false;
    return build_stream_jpeg_rgb(ctx, rgb, out);
}

// 由已解码的 RGB 帧出流（缓存命中路径）：缩放补齐 + 编码，不再重复解码。
bool build_stream_jpeg_rgb(AppContext& ctx, const csrc::Camera::RgbFrame& rgb,
                           std::vector<uint8_t>& out) {
    const int ow = ctx.config.camera.stream_width;
    const int oh = ctx.config.camera.stream_height;
    const int q = ctx.config.camera.stream_quality;
    if (ow <= 0 || oh <= 0 || q <= 0 || rgb.data.empty()) return false;
    const int w = rgb.w, h = rgb.h;
    if (w <= 0 || h <= 0) return false;

    if (w == ow && h == oh) {
        return csrc::Camera::rgb_to_jpeg(rgb.data.data(), w, h, q, out);
    }
    std::vector<uint8_t> box((size_t)ow * oh * 3);
    if (!csrc::Camera::letterbox_rgb(rgb.data.data(), w, h, box.data(), ow, oh))
        return false;
    return csrc::Camera::rgb_to_jpeg(box.data(), ow, oh, q, out);
}

// ═══════════════════════ 单帧推理服务 ═══════════════════════

namespace {

/// 校验临时文件（CviModel 魔数 + 大小上限）后原子换入最终路径；失败时删掉临时文件并填 err。
bool install_model_file(const std::string& tmp_path, const std::string& final_path,
                        long long max_bytes, long long& size_out, std::string& err) {
    std::ifstream f(tmp_path, std::ios::binary);
    if (!f) {
        err = "临时文件打不开";
        return false;
    }
    char magic[8] = {0};
    f.read(magic, sizeof magic);
    f.seekg(0, std::ios::end);
    const long long sz = (long long)f.tellg();
    f.close();
    if (sz < (long long)sizeof magic || std::string(magic, sizeof magic) != "CviModel") {
        err = "不是 cvimodel（文件头不是 CviModel）";
        std::remove(tmp_path.c_str());
        return false;
    }
    if (sz > max_bytes) {
        err = "模型过大：" + std::to_string(sz / (1024 * 1024)) + "MB，上限 " +
              std::to_string(max_bytes / (1024 * 1024)) + "MB";
        std::remove(tmp_path.c_str());
        return false;
    }
    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        err = std::string("换入失败：") + std::strerror(errno);
        std::remove(tmp_path.c_str());
        return false;
    }
    size_out = sz;
    return true;
}

}  // namespace

std::string model_dir(AppContext& ctx) { return ctx.app_dir + "/demo/models"; }

std::string model_path(AppContext& ctx, const std::string& name) {
    return model_dir(ctx) + "/" + name + ".cvimodel";
}

std::string demo_script_path(AppContext& ctx, const std::string& name) {
    return ctx.app_dir + "/demo/" + name + ".lua";
}

bool script_file_exists(AppContext& ctx, const std::string& name) {
    return access(demo_script_path(ctx, name).c_str(), F_OK) == 0;
}

csrc::Json save_model_upload(AppContext& ctx, const std::string& name, const std::string& content) {
    csrc::Json j;
    const std::string final_path = model_path(ctx, name);
    const std::string tmp_path = final_path + ".part";   // 先落 .part 再原子换入
    // 目录得自己建，而且**要递归**：`demo/` 一级在 OTA 之后一定在（包里带着），
    // 但裸 mkdir() 只建一层、返回值还容易被忽略，最后表现成"临时文件写不开"，白查。
    if (!csrc::ensure_dir(model_dir(ctx))) {
        j["ok"] = false;
        j["error"] = "建模型目录失败：" + model_dir(ctx);
        return j;
    }

    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            j["ok"] = false;
            j["error"] = "临时文件写不开：" + tmp_path;
            return j;
        }
        f.write(content.data(), (std::streamsize)content.size());
        f.close();
        if (!f) {
            std::remove(tmp_path.c_str());
            j["ok"] = false;
            j["error"] = "写临时文件失败（磁盘满？）";
            return j;
        }
    }

    long long sz = 0;
    std::string err;
    if (!install_model_file(tmp_path, final_path, capp::kMaxRequestBody, sz, err)) {
        j["ok"] = false;
        j["error"] = err;
        return j;
    }
    CAM_INFO("[models] 模型已上传 %s（%lld KB）", final_path.c_str(), sz / 1024);
    j["ok"] = true;
    j["name"] = name;
    j["path"] = final_path;
    j["size"] = csrc::Json((int64_t)sz);
    return j;
}

bool valid_model_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (name == "." || name == "..") return false;
    for (char ch : name) {
        const char c = (char)ch;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

/// 取一帧跑一次推理 → 框列表（原图像素坐标）。`/api/detect` 与脚本原语共用同一条链：
/// 模型懒加载 / 文件变了重载 / 取原生帧 / 推理。错误串与对外契约保持一致。
bool detect_boxes(AppContext& ctx, const std::string& model_name, const csrc::DecodeOptions& opt,
                  std::vector<csrc::Detection>& out, int& frame_w, std::string& err) {
    out.clear();
    frame_w = 0;

    // 锁罩住"换模型 + 推理"整段：TPU 是单实例，YoloDetector 非线程安全；
    // 半路换模型或两个请求并发进来都会出问题。
    std::lock_guard<std::mutex> lk(ctx.detect_mu);

    if (!ctx.detector) ctx.detector.reset(new csrc::YoloDetector());
    // 模型只有一个来源：model_path()（= $AKA_HOME/demo/models/<名字>.cvimodel）
    const std::string path = model_path(ctx, model_name);
    // 文件被换过（重新下载覆盖）也要重载 —— 一次 stat 的开销，换"覆盖即生效"。
    struct stat st {};
    const bool have = (stat(path.c_str(), &st) == 0);
    const bool changed = have && (st.st_mtime != ctx.detect_mtime ||
                                  (long long)st.st_size != ctx.detect_size);
    if (!ctx.detector->loaded() || ctx.detect_model != model_name || changed) {
        if (!ctx.detector->load(path, err)) {
            ctx.detect_model.clear();
            return false;
        }
        ctx.detect_model = model_name;
        ctx.detect_mtime = have ? st.st_mtime : 0;
        ctx.detect_size = have ? (long long)st.st_size : 0;
    }

    if (!ensure_camera(ctx)) {
        err = "camera not available";
        return false;
    }
    // 取帧用**原生采集宽度**（camera.width），不是浏览器的 stream_width。
    // stream_width 是为了省浏览器带宽而降采样的（默认 320），拿它喂 640x480 的模型
    // 等于先把画面砍掉一半再放大回去（白丢分辨率）；更要命的是返回的框就落在那张
    // 320 宽帧的坐标系里，而 /api/camera/snapshot 给的是原生 640 宽帧 —— 两者差一倍，
    // 调用方把框画到快照上就会整体跑偏（实测框跑到画面左上角的背景上）。
    // 用 camera.width 还能和板载屏显示的 decode_max_w 同档，共用同一次解码。
    csrc::Camera::RgbFrame rgb;
    if (!ctx.camera.latest_rgb(ctx.config.camera.width, rgb) || rgb.data.empty()) {
        err = "no frame";
        return false;
    }
    frame_w = rgb.w;
    // 取帧（含解码，走 Camera::latest_rgb 的共享缓存）单独计一下：
    // 慢在"取帧"还是"推理"，决定了该优化哪条路。
    const auto t_det0 = std::chrono::steady_clock::now();
    const bool ok = ctx.detector->detect(rgb.data.data(), rgb.w, rgb.h, opt, out, err);
    const double det_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t_det0).count();
    CAM_DEBUG("[detect/frame] 取帧(含解码) 已完成，detect() 耗时 %.1fms", det_ms);
    return ok;
}

csrc::DecodeOptions decode_options(double conf, double iou) {
    csrc::DecodeOptions opt;   // 默认 conf=0.25 / iou=0.45
    // 0 = 调用方没给；给了就夹到 0.01~0.99（0 会"什么都留"，1 会"什么都不要"，
    // 两个极端都不是能用阈值，夹住比报错省事）
    if (conf > 0) opt.conf = (float)std::min(0.99, std::max(0.01, conf));
    if (iou > 0) opt.iou = (float)std::min(0.99, std::max(0.01, iou));
    return opt;
}

csrc::Json detect_once(AppContext& ctx, const std::string& model_name,
                       const csrc::DecodeOptions& opt) {
    csrc::Json j;
    std::vector<csrc::Detection> dets;
    int frame_w = 0;
    std::string err;
    if (!detect_boxes(ctx, model_name, opt, dets, frame_w, err)) {
        j["ok"] = false;
        j["error"] = err;
        return j;
    }

    csrc::Json boxes(csrc::Json::Type::Array);
    for (const auto& d : dets) {
        csrc::Json b;
        b["x1"] = csrc::Json((double)d.box.x1);
        b["y1"] = csrc::Json((double)d.box.y1);
        b["x2"] = csrc::Json((double)d.box.x2);
        b["y2"] = csrc::Json((double)d.box.y2);
        boxes.push_back(b);
    }
    j["ok"] = true;
    j["count"] = csrc::Json((int64_t)dets.size());
    j["boxes"] = boxes;
    return j;
}

// ═══════════════════════ 板载屏显示服务 ═══════════════════════

// 启动屏显示（幂等）。摄像头未开时按需打开（屏要画面就得有摄像头）——
// 摄像头打开后 ensure_camera 内部也会自动启动显示，两条路都通。
bool ensure_display(AppContext& ctx) {
    if (ctx.display.running()) return true;
    if (!ctx.config.display.enabled) return false;
    // 显示依赖摄像头最新帧（复用 Camera 单例，不额外占用设备）
    if (!ensure_camera(ctx)) {
        CAM_WARN("[display] camera unavailable — screen off");
        return false;
    }
    return start_display_locked_on_camera(ctx);
}

void close_display(AppContext& ctx) { ctx.display.stop(); }

csrc::Json display_config(AppContext& ctx) {
    csrc::Json j;
    j["enabled"] = ctx.config.display.enabled;      // 配置/当前开关状态
    j["running"] = ctx.display.running();           // 显示线程是否真在跑
    j["available"] = ctx.display.available();       // 有没有 /dev/fb0
    j["scale"] = csrc::Json((int64_t)ctx.config.display.scale);
    j["fps"] = csrc::Json((int64_t)ctx.config.display.fps);
    if (ctx.display.running()) {
        const csrc::ScreenDisplay::Stats st = ctx.display.stats();
        j["region_w"] = csrc::Json((int64_t)st.out_w);
        j["region_h"] = csrc::Json((int64_t)st.out_h);
        j["frames"] = csrc::Json((int64_t)st.frames);
    }
    return j;
}

csrc::Json set_display_enabled(AppContext& ctx, bool enabled) {
    ctx.config.display.enabled = enabled;
    csrc::Json j;
    if (!enabled) {
        close_display(ctx);      // 立刻停屏（关摄像头路径也会调，幂等）
        CAM_INFO("[display] 运行时关闭屏显示");
    } else {
        // 打开：摄像头已开才会真的起屏（起不来不算错误，如实回报 running=false）
        ensure_display(ctx);
        CAM_INFO("[display] 运行时打开屏显示 (running=%d)", (int)ctx.display.running());
    }
    j["ok"] = true;
    j["enabled"] = ctx.config.display.enabled;
    j["running"] = ctx.display.running();
    return j;
}

// ═══════════════════════ 状态上报 ═══════════════════════

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
