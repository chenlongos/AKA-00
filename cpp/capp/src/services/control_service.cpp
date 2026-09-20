// 控制服务：底盘动作、机械臂动作、原始指令
//
// 由 capp/src/services.cpp 按域拆出来（对应 app/services/*.py 的分法）。
// 对应 app/services/control_service.py。文件内私有的是几个定时停/等待辅助。
// 声明都在 capp/context.hpp（那一份是按域分节的伞头文件，调用方只 include 它）。

#include "capp/context.hpp"

#include "csrc/log.hpp"
#include <chrono>
#include <cmath>
#include <thread>
#include <unistd.h>

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

// 这两个**调用时 arm_mu 已经在手里**（apply_arm_action 先 try_lock 再交给线程接手），
// 所以它们自己不再加锁。

// 这两个**调用时 arm_mu 已经在手里**（apply_arm_action 先 try_lock 再交给线程接手），
// 所以它们自己不再加锁。
void do_grab(AppContext& ctx) {
    ctx.gripper->close();
    ctx.collector.set_gripper_target(0);
}

void do_release(AppContext& ctx) {
    ctx.gripper->open();
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

ArmResult apply_arm_action(AppContext& ctx, const std::string& action) {
    if (action != "grab" && action != "release") return ArmResult::NotArm;

    // **不排队**：夹爪那套序列要 ~3.5s（ZP10S：伸下去→夹→抬起）。以前每来一次请求就
    // spawn 一个后台线程去抢 arm_mu —— 连点多次 grab 就是"排了一串队挨个执行"，
    // 表现是"点了很多次，它就一直夹取"。正忙就跳过这一次，并把"忙"如实报给调用方。
    if (!ctx.arm_mu.try_lock()) return ArmResult::Busy;

    const bool is_grab = (action == "grab");
    if (is_grab) {
        ctx.collector.set_gripper_target(1);
        ctx.collector.set_gripper_status("closed");
    }
    try {
        std::thread([&ctx, is_grab] {
            std::lock_guard<std::mutex> lk(ctx.arm_mu, std::adopt_lock);   // 接手已持有的锁
            if (is_grab) do_grab(ctx); else do_release(ctx);
        }).detach();
    } catch (...) {
        ctx.arm_mu.unlock();   // 线程没起成就把锁还回去
        return ArmResult::NotArm;
    }
    return ArmResult::Accepted;
}

csrc::Json execute_action(AppContext& ctx, const std::string& action, int speed,
                          double milliseconds, bool wait_done) {
    cancel_pending_stop(ctx);
    int64_t seq = bump_motion_seq(ctx);
    CAM_INFO("[control] action=%s speed=%d ms=%.0f wait=%d", action.c_str(), speed,
             milliseconds, (int)wait_done);

    bool handled = apply_base_action(ctx, action, speed);
    if (!handled) {
        switch (apply_arm_action(ctx, action)) {
            case ArmResult::Accepted: handled = true; break;
            case ArmResult::Busy: {
                csrc::Json err;
                err["status"] = "error";
                err["message"] = "夹爪正忙：上一段动作还没做完（这次没做，也没排队）";
                return err;
            }
            case ArmResult::NotArm: break;
        }
    }
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
        // 换算单位：**API 的 distance 是厘米**（文档口径，前端/curl 都按 cm 传），
        // 固件的 CMD_MOVE_DISTANCE 要毫米。
        // 原来这里直接把 cm 当 mm 发下去了 —— `?distance=30` 实际只走 3cm，差 10 倍。
        target = (int32_t)std::llround(value * 10.0);
        unit = "cm";
    } else {
        target = (int32_t)std::llround(value * 10);  // 转向：固件收 0.1° 为单位
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
            // 无状态源（底盘没连上 / 链路等待中掉线）：无法精确判定，
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
    // 统一的"执行完成"标志：三个会动的入口（距离/角度、走几秒、跑 demo）都给这个布尔，
    // 客户端只认它就行；细节看 status / state / message
    ok["completed"] = (outcome == 2 || outcome == -1);
    ok["mode"] = "esp32";
    ok["direction"] = direction;   // forward/backward/left/right —— 客户端据此知道走的是哪边
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


// 内部：在"摄像头已开"的前提下启动屏显示（不再回调 ensure_camera，避免递归）

}  // namespace capp
