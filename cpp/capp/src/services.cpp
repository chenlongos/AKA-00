// capp/services.cpp — 服务层实现
//
// 对应 app/services/control_service.py + camera_service.py + status_reporter.py
// （行为/JSON 契约与原 Python app 保持一致）

#include "capp/context.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "csrc/http_client.hpp"
#include "csrc/log.hpp"
#include "csrc/system_utils.hpp"

namespace capp {

namespace {

// ── 定时停线程管理 ──

void cancel_pending_stop(AppContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.timer_mu);
    if (ctx.timer_thread) {
        ctx.timer_cancel = true;
        ctx.timer_thread->join();
        delete ctx.timer_thread;
        ctx.timer_thread = nullptr;
    }
}

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

void do_grab(AppContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.arm_mu);
    ctx.gripper->close();
    ctx.collector.set_gripper_target(0);
}

void do_release(AppContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.arm_mu);
    ctx.gripper->open();
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

    bool real = ctx.config.motor.backend != "dev" || ctx.config.arm.backend != "dev";
    CAM_INFO("[app] services ready (motor=%s arm=%s)%s",
             ctx.config.motor.backend.c_str(), ctx.config.arm.backend.c_str(),
             real ? "" : " — all mock (backend=dev)");
    return real;
}

// ═══════════════════════ 控制服务 ═══════════════════════

csrc::Json execute_action(AppContext& ctx, const std::string& action, int speed,
                          double milliseconds, bool wait_done) {
    cancel_pending_stop(ctx);
    int64_t seq = bump_motion_seq(ctx);

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
    ctx.motor_pair->move_distance((uint8_t)d, (uint8_t)sp, target);

    // 同步：等 ESP32 闭环跑完（转速持续为 0）再返回确认 ACK。
    // 距离/转角大时该请求会挂几秒~几十秒，属预期（客户端勿设短超时）。
    auto t0 = std::chrono::steady_clock::now();
    bool ever_moved = false;
    bool stopped = wait_stationary(ctx, 30.0, 0.35, ever_moved);
    double elapsed_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();

    csrc::Json ok;
    ok["status"] = stopped ? "completed" : "timeout";
    ok["mode"] = "esp32";
    ok["target"] = value;
    ok["unit"] = unit;
    ok["moved"] = ever_moved;
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
    bool enabled = ctx.config.motor.backend != "dev";
    j["enabled"] = enabled;
    bool connected = false;
    std::string state = "disabled";
    int attempts = 0;
    std::string error;
    if (ctx.motor_link) {
        csrc::MotorLinkStatus st = ctx.motor_link->link_status();
        connected = st.connected;
        state = st.state;
        attempts = st.attempts;
        error = st.error;
    }
    j["connected"] = connected;
    j["state"] = state;
    j["attempts"] = csrc::Json((int64_t)attempts);
    j["error"] = error;
    return j;
}

// ═══════════════════════ 摄像头服务 ═══════════════════════

bool ensure_camera(AppContext& ctx) {
    if (ctx.camera_on) return true;
    bool ok = ctx.camera.open(ctx.config.camera.width, ctx.config.camera.height,
                              ctx.config.camera.fps);
    ctx.camera_on = ok;
    return ok;
}

void close_camera(AppContext& ctx) {
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
bool build_stream_jpeg(AppContext& ctx, const csrc::Camera::Frame& f,
                       std::vector<uint8_t>& out) {
    const int ow = ctx.config.camera.stream_width;
    const int oh = ctx.config.camera.stream_height;
    const int q = ctx.config.camera.stream_quality;
    if (ow <= 0 || oh <= 0 || q <= 0 || f.data.empty()) return false;

    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    if (csrc::Camera::is_jpeg(f.data.data(), f.data.size())) {
        // libjpeg 缩放解码：输出宽 ≤ ow（保持宽高比，1/1..1/8 整数降采样）
        if (!csrc::Camera::jpeg_to_rgb(f.data.data(), f.data.size(), w, h, rgb, ow))
            return false;
    } else if (f.w > 0 && f.h > 0) {
        // YUYV 兜底：先转 RGB，再缩放
        w = f.w;
        h = f.h;
        rgb.resize((size_t)w * h * 3);
        csrc::Camera::yuyv_to_rgb(f.data.data(), w, h, rgb.data());
    } else {
        return false;
    }
    if (w <= 0 || h <= 0) return false;

    if (w == ow && h == oh) {
        return csrc::Camera::rgb_to_jpeg(rgb.data(), w, h, q, out);
    }
    std::vector<uint8_t> box((size_t)ow * oh * 3);
    if (!csrc::Camera::letterbox_rgb(rgb.data(), w, h, box.data(), ow, oh))
        return false;
    return csrc::Camera::rgb_to_jpeg(box.data(), ow, oh, q, out);
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
