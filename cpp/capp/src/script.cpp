// capp/script.cpp — 内置动作状态机宿主（原 Lua 流程脚本迁进 C++）
//
// ── 为什么有这一层 ──
// "看 → 对准 → 靠近 → 抓"这类**流程**天生要反复调参（阈值、脉冲时长、速度…）。
// 原先写在 Lua 脚本里（改一行存盘重跑、不用重编）。现在流程迁进 C++：
//   · 动作硬编码成内置状态机（grab / approach），差异常量以「卡片 JSON 可选字段 +
//     C++ 出厂默认」外置（见 TuningParams / load_tuning）；最常改的 speed /
//     target_size / turn_speed / mode 本来就走卡片参数，不重编照样能调。
//   · 原语与安全兜底仍留在宿主（快、稳）。
//
// ── 红线：安全兜底不交给状态机 ──
// 状态机只能调用「有上限的原语」，下面这些由宿主强制：
//   · 速度上限       drive/forward/... 的参数一律 clamp 到 ±kMaxSpeed
//   · 执行方式       params.mode：once（跑一遍，**最多 kMaxOnceSeconds**，到点宿主自己
//                    收工）/ loop（跑完接着跑，**没有总时长上限**，停不停由 stop / 人的指令
//                    接管 / 服务退出决定）
//   · 被人的指令取代 有人推进了 motion_seq（摇杆/HTTP 指令）→ 立刻打断，交还控制权
//   · stop / 服务退出 / 底盘掉线 —— 同上，立刻退出
//   · 卡死/无响应     每个原语入口都查一次打断条件（C++ 无脚本死循环风险）
// 退出时宿主兜底把电机停回静止（状态机自己忘了停也一样）。

#include "capp/context.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <thread>
#include <vector>

#include "csrc/log.hpp"

namespace capp {

namespace {

// 上限类常量（状态机改不了）
// 速度硬上限（%）：卡片/ demo 配置给再大的数，到这里一律砍成 ±这个值。
// 原来定的 35 实测不够用（demo 需要的直线速度在 40~70 之间），所以放宽到 70。
// 留着它是因为这是唯一拦在"乱发速度"和电机之间的东西，别顺手删。
constexpr int kMaxSpeed = 70;
// **执行一次（mode=once）的最大执行时间**：到点宿主自己收工（等价于按了停止 —— 停电机、
// 状态落 aborted、消息写清楚原因）。循环执行（loop）**不设这个上限**。
constexpr double kMaxOnceSeconds = 300.0;   // 5 分钟
// 循环执行时每轮之间的间隔（毫秒）：停一下车、也给 stop/人的指令留响应窗口
constexpr int kLoopGapMs = 200;
constexpr int kSleepStepMs = 50;            // sleep_ms 切段睡，便于被中断
// ESP32 固件的"速度命令看门狗"：单发一条只转 0.2~0.9s 就自己停。所以只要状态机处在
// "有速度"状态，宿主就替它重发。
constexpr int kKeepaliveMs = 80;
// grab 之后等 ZP10S 那套"伸下去→夹→抬起"走完（约 3.5s）
constexpr int kGrabWaitMs = 4000;

/// 一次运行的全部状态：宿主与状态机共享
struct RunCtx {
    AppContext* ctx = nullptr;
    std::chrono::steady_clock::time_point t0;

    int64_t own_seq = 0;    // 本次运行最近一次推进的指令代际号（用来发现"被接管"）
    bool claimed = false;   // 是否动过电机（没动过就不该误判"被接管"）
    bool once = false;      // 执行一次（true 才受 kMaxOnceSeconds 管；loop 不管）
    bool moving = false;    // 发过速度且还没回静止 → 收尾兜底要停
    int last_left = 0, last_right = 0;   // 当前发给底盘的速度（保活重发用）
    long long last_keepalive_ms = 0;
    std::string reason;     // 打断原因；空 = 正常结束
};

/// 打断/失败信号：check_interrupt / fail 抛出，由工作线程顶层接住走统一收尾。
/// 替代原先 Lua 的 luaL_error。
struct Interrupt {};

long long now_ms(const RunCtx* r) {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - r->t0).count();
}

/// 底盘是否真在线。掉线时 `active_` 置空 —— 指令被丢弃、`/api/motor/status` 报
/// connected=false，所以每一拍都得主动查，否则就是"对着空气跑"。
bool motor_ok(AppContext& ctx) { return motor_status_json(ctx).getb("connected", false); }

/// 只**判定**打断（命中就把原因写进 r->reason），不抛 —— 抛由调用方决定。
/// 顺序有意为之：先看人为的停止，再看接管/硬件，最后才是我们自己的时限。
void detect_interrupt(RunCtx* r) {
    if (!r->reason.empty()) return;   // 已经判定过了
    if (r->ctx->script_abort) {
        r->reason = "aborted: 收到停止请求";
    } else if (r->ctx->shutdown) {
        r->reason = "aborted: 服务退出";
    } else if (r->claimed && motion_seq_now(*r->ctx) != r->own_seq) {
        r->reason = "superseded: 被新的运动指令取代（人接管）";
    } else if (!motor_ok(*r->ctx)) {
        r->reason = "motor: 底盘掉线";
    } else if (r->once && now_ms(r) >= (long long)(kMaxOnceSeconds * 1000.0)) {
        r->reason = "timeout: 到最大执行时间（" +
                    std::to_string((long long)(kMaxOnceSeconds / 60.0)) + " 分钟）";
    }
    if (!r->reason.empty()) CAM_WARN("[action] 打断：%s", r->reason.c_str());
}

/// 原语用：判定 + 命中就抛（安全路径 —— 停止/掉线/被接管都靠它）。
void check_interrupt(RunCtx* r) {
    detect_interrupt(r);
    if (!r->reason.empty()) throw Interrupt{};
}

/// 保活：底盘固件要持续收到速度命令才肯转。sleep_ms 里每段调一次 —— 于是不管
/// 状态机在等还是在动，只要还有速度就一直转。
void keepalive_tick(RunCtx* r) {
    if (r->last_left == 0 && r->last_right == 0) return;
    const long long now = now_ms(r);
    if (now - r->last_keepalive_ms < kKeepaliveMs) return;
    r->last_keepalive_ms = now;
    r->ctx->motor_pair->set_speed(r->last_left, r->last_right);
}

/// 声明"这次动作属于本运行"：取消挂起的定时停 + 推进指令代际号。
/// 之后若有人再动电机，代际号就会变，check_interrupt 立刻发现。
void claim_motion(RunCtx* r) {
    cancel_pending_stop(*r->ctx);
    r->own_seq = bump_motion_seq(*r->ctx);
    r->claimed = true;
}

void publish(RunCtx* r, const char* action) {
    std::lock_guard<std::mutex> lk(r->ctx->script_mu);
    r->ctx->script_calls++;
    r->ctx->script_action = action;
}

void set_speed_checked(RunCtx* r, int left, int right, const char* action) {
    const int l = std::max(-kMaxSpeed, std::min(kMaxSpeed, left));
    const int rr = std::max(-kMaxSpeed, std::min(kMaxSpeed, right));
    claim_motion(r);
    r->ctx->motor_pair->set_speed(l, rr);
    r->ctx->collector.set_target_speed(l, rr);
    r->moving = (l != 0 || rr != 0);
    r->last_left = l;
    r->last_right = rr;
    r->last_keepalive_ms = now_ms(r);
    publish(r, action);
}

// ────────────────────────── 原语（C++，读 RunCtx） ──────────────────────────

void prim_forward(RunCtx* r, int s) { check_interrupt(r); set_speed_checked(r, s, s, "forward"); }
void prim_back(RunCtx* r, int s) { check_interrupt(r); set_speed_checked(r, -s, -s, "back"); }
void prim_turn_left(RunCtx* r, int s) { check_interrupt(r); set_speed_checked(r, -s, s, "turn_left"); }
void prim_turn_right(RunCtx* r, int s) { check_interrupt(r); set_speed_checked(r, s, -s, "turn_right"); }
void prim_standby(RunCtx* r) {
    check_interrupt(r);
    r->ctx->motor_pair->set_speed(0, 0);
    r->ctx->collector.set_target_speed(0, 0);
    r->moving = false;
    r->last_left = r->last_right = 0;
    publish(r, "standby");
}
void prim_brake(RunCtx* r) {
    check_interrupt(r);
    r->ctx->motor_pair->brake();
    r->ctx->collector.set_target_speed(0, 0);
    r->moving = false;
    r->last_left = r->last_right = 0;
    publish(r, "brake");
}
/// sleep_ms(ms)：切段睡，每段都查打断 —— 所以 /api/demo/stop 不用等状态机醒
void prim_sleep_ms(RunCtx* r, long long ms) {
    check_interrupt(r);
    if (ms < 0) ms = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        check_interrupt(r);
        keepalive_tick(r);   // 等待期间不能让底盘固件的看门狗把车停了
        std::this_thread::sleep_for(std::chrono::milliseconds(kSleepStepMs));
    }
}
void prim_grab(RunCtx* r) {
    check_interrupt(r);
    apply_arm_action(*r->ctx, "grab");     // 内部持 arm_mu；ZP10S 是约 3.5s 的整段序列
    publish(r, "grab");
}
/// detect(model, conf, iou) → 填 dets/frame_w；硬错误（非 "no frame"）返回 false + err
bool prim_detect(RunCtx* r, const std::string& model, double conf, double iou,
                 std::vector<csrc::Detection>& dets, int& frame_w, std::string& err) {
    check_interrupt(r);
    if (!valid_model_name(model)) {
        err = "模型名非法（只允许字母数字与 _ - .）：" + model;
        return false;
    }
    if (!detect_boxes(*r->ctx, model, decode_options(conf, iou), dets, frame_w, err)) {
        // "no frame"（相机刚开、这一拍还没出帧）对视觉伺服来说等价于"这拍没看到目标"，
        // 返回空列表让状态机走它自己的丢帧分支；其余才是硬错误。
        if (err != "no frame") return false;
        dets.clear();
    }
    publish(r, "detect");
    return true;
}
/// note(key, value)：发布一个可观测字段给 /api/demo/status
void prim_note(RunCtx* r, const std::string& k, const std::string& v) {
    std::lock_guard<std::mutex> lk(r->ctx->script_mu);
    auto& notes = r->ctx->script_notes;
    for (auto& kv : notes) {
        if (kv.first == k) { kv.second = v; return; }
    }
    notes.emplace_back(k, v);
}
void prim_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    CAM_INFO("[action] %s", buf);
}
/// fail(msg)：状态机主动判定失败 → 宿主把状态报成 failed 并停车
[[noreturn]] void prim_fail(RunCtx* r, const std::string& msg) {
    r->reason = "failed: " + msg;
    throw Interrupt{};
}

// ────────────────────────── 低频调参常量（卡片 JSON 可覆盖） ──────────────────────────
//
// 字段名/默认值严格照搬原 grab.lua / approach.lua。最常改的 speed/target_size/turn_speed/
// mode 走运行参数（不在此列）；这里放的是实测调过、改动很低的判据常量。
struct TuningParams {
    double center_margin = 80;     // 走的时候用：偏出这个范围就来一次大转向
    double turn_pulse_k = 0.5;     // 转向脉冲系数：ms/px（偏离越多转越久）
    double turn_pulse_min = 300;   // 转向脉冲下限（必须 > 电机启动时间 ~250ms）
    double turn_pulse_max = 400;
    double fwd_pulse_ms = 600;     // 前进脉冲（必须 > 电机启动时间）
    double align_margin = 25;      // 抓的时候用：对到 grab_offset 附近这么小才敢夹
    double too_close_k = 1.5;      // 框宽超过目标这个倍数 = 凑得太近
    double back_pulse_k = 2.0;     // 后退脉冲系数：ms/px（超出的像素数）
    double back_pulse_min = 300;
    double back_pulse_max = 700;
    double grab_offset = 50;       // 夹爪相对画面中心的正偏移（只能实测调）
    double fine_pulse_max = 500;   // 精调转向的脉冲上限
    double lost_ms = 1500;         // 连续多久看不到目标就收工
    double loop_gap_ms = 30;       // 每步之间喘口气，让相机出新帧
    double conf = 0;               // detect 阈值：0 = 用默认（0.25 / 0.45）
    double iou = 0;
};

/// 从 params 覆盖出厂默认：getd 在键缺失/非数字时返回默认值，故逐项 `t.x = getd(k, t.x)`。
TuningParams load_tuning(const csrc::Json& p) {
    TuningParams t;
    t.center_margin = p.getd("center_margin", t.center_margin);
    t.turn_pulse_k = p.getd("turn_pulse_k", t.turn_pulse_k);
    t.turn_pulse_min = p.getd("turn_pulse_min", t.turn_pulse_min);
    t.turn_pulse_max = p.getd("turn_pulse_max", t.turn_pulse_max);
    t.fwd_pulse_ms = p.getd("fwd_pulse_ms", t.fwd_pulse_ms);
    t.align_margin = p.getd("align_margin", t.align_margin);
    t.too_close_k = p.getd("too_close_k", t.too_close_k);
    t.back_pulse_k = p.getd("back_pulse_k", t.back_pulse_k);
    t.back_pulse_min = p.getd("back_pulse_min", t.back_pulse_min);
    t.back_pulse_max = p.getd("back_pulse_max", t.back_pulse_max);
    t.grab_offset = p.getd("grab_offset", t.grab_offset);
    t.fine_pulse_max = p.getd("fine_pulse_max", t.fine_pulse_max);
    t.lost_ms = p.getd("lost_ms", t.lost_ms);
    t.loop_gap_ms = p.getd("loop_gap_ms", t.loop_gap_ms);
    t.conf = p.getd("conf", t.conf);
    t.iou = p.getd("iou", t.iou);
    return t;
}

// ────────────────────────── 主状态机（逐行移植自 grab.lua / approach.lua） ──────────────────────────

/// 运行参数：动作 + 必填/常用参数 + 调参常量。
struct RunParams {
    std::string model;
    double target = 0;      // target_size（原图像素框宽）
    int speed = 20;         // 直线速度
    int turn_speed = 20;    // 转弯速度（没配就跟直线一样）
    TuningParams t;
};

/// 跑一遍追踪循环（对应 lua 的 `while true do ... end`）。正常到位/丢目标时返回结论字符串；
/// 打断/失败经 check_interrupt / prim_fail 抛 Interrupt。
std::string run_action_once(RunCtx* r, const ActionDef& act, const RunParams& p) {
    const TuningParams& t = p.t;
    long long last_seen = now_ms(r);

    while (true) {
        check_interrupt(r);

        std::vector<csrc::Detection> dets;
        int frame_w = 0;
        std::string err;
        if (!prim_detect(r, p.model, t.conf, t.iou, dets, frame_w, err)) {
            // 推理本身失败（模型没了/相机不可用）——重试没意义，直接收工
            prim_fail(r, "推理失败：" + err);
        }

        // 选面积最大的框（最近的优先）
        const csrc::Detection* best = nullptr;
        double best_area = -1;
        for (const auto& d : dets) {
            const double w = d.box.x2 - d.box.x1, h = d.box.y2 - d.box.y1;
            const double area = w * h;
            if (!best || area > best_area) { best = &d; best_area = area; }
        }

        if (!best) {
            const long long lost = now_ms(r) - last_seen;
            prim_note(r, "lost_ms", std::to_string(lost));
            if ((double)lost > t.lost_ms) {
                prim_standby(r);
                return "目标丢失（" + std::to_string(lost) + "ms 没看到目标）";
            }
            prim_standby(r);                 // 丢帧就原地等，不搜索
            prim_sleep_ms(r, (long long)t.loop_gap_ms);
            continue;
        }

        last_seen = now_ms(r);
        const double w = best->box.x2 - best->box.x1;
        const double cx = (best->box.x1 + best->box.x2) * 0.5;
        const double offset = cx - frame_w / 2.0;      // 正 = 目标偏右
        prim_note(r, "box_w", std::to_string((long long)std::floor(w)));
        prim_note(r, "offset", std::to_string((long long)std::floor(offset)));

        // 转向方向一律是"把目标送到 grab_offset 那个位置"：目标偏左就左转（视角随之右移）
        const double align_err = offset - t.grab_offset;
        if (w > p.target * t.too_close_k) {
            // 太近：退一小段再重新判。退多久按超出的像素数算。
            const double over = w - p.target * t.too_close_k;
            long long back_ms = (long long)std::floor(t.back_pulse_k * over);
            back_ms = std::max((long long)t.back_pulse_min,
                               std::min((long long)t.back_pulse_max, back_ms));
            prim_back(r, p.speed);
            prim_sleep_ms(r, back_ms);
            prim_standby(r);
            prim_log("太近：框宽 %lldpx（超出到位区间 %lldpx）→ 后退 %lldms",
                     (long long)std::floor(w), (long long)std::floor(over), back_ms);
        } else if (w >= p.target && std::fabs(align_err) <= t.align_margin) {
            // 够大 + 对准夹爪 → 停稳
            prim_brake(r);
            prim_log("到位：框宽 %lldpx（目标 %lld）偏移 %lld（夹爪位 %lld，误差 %lld）",
                     (long long)std::floor(w), (long long)p.target, (long long)std::floor(offset),
                     (long long)std::floor(t.grab_offset), (long long)std::floor(align_err));
            if (act.do_grab) {
                prim_grab(r);
                prim_sleep_ms(r, kGrabWaitMs);   // 等 ZP10S 那套"伸下去→夹→抬起"走完（约 3.5s）
                return "已抓取（是否夹到请看实物：夹爪没有反馈）";
            }
            return "已到位（未抓取）：框宽 " + std::to_string((long long)std::floor(w)) +
                   "px，偏移 " + std::to_string((long long)std::floor(offset)) +
                   "，夹爪位 " + std::to_string((long long)std::floor(t.grab_offset));
        } else if (std::fabs(offset) > t.center_margin) {
            // 还差得远：大脉冲转向
            long long pulse = (long long)std::floor(t.turn_pulse_k * std::fabs(align_err));
            pulse = std::max((long long)t.turn_pulse_min,
                             std::min((long long)t.turn_pulse_max, pulse));
            if (align_err < 0) prim_turn_left(r, p.turn_speed);
            else prim_turn_right(r, p.turn_speed);
            prim_sleep_ms(r, pulse);
            prim_standby(r);
        } else if (w >= p.target) {
            // 距离够了但没对准夹爪：小脉冲精调（别转过头）
            long long pulse = (long long)std::floor(t.turn_pulse_k * std::fabs(align_err));
            pulse = std::max((long long)t.turn_pulse_min,
                             std::min((long long)t.fine_pulse_max, pulse));
            prim_log("精调：偏移 %lld → 对准 %lld（误差 %lld，脉冲 %lldms）",
                     (long long)std::floor(offset), (long long)std::floor(t.grab_offset),
                     (long long)std::floor(align_err), pulse);
            if (align_err < 0) prim_turn_left(r, p.turn_speed);
            else prim_turn_right(r, p.turn_speed);
            prim_sleep_ms(r, pulse);
            prim_standby(r);
        } else {
            prim_forward(r, p.speed);
            prim_sleep_ms(r, (long long)t.fwd_pulse_ms);
            prim_standby(r);
        }
        prim_sleep_ms(r, (long long)t.loop_gap_ms);
    }
}

// ────────────────────────── 结果归类 ──────────────────────────

/// reason → (state, message)。空 reason = 正常结束。
void classify(const std::string& reason, const std::string& ret, std::string& state,
              std::string& message) {
    if (reason.empty()) {
        state = "done";
        message = ret.empty() ? "动作正常结束" : ret;
        return;
    }
    // "timeout:"（执行一次到点收工）落进 aborted —— 它不是失败，是被宿主收工
    if (reason.rfind("failed:", 0) == 0 ||
        reason.rfind("motor:", 0) == 0 || reason.rfind("error:", 0) == 0) {
        state = "failed";
    } else {
        state = "aborted";   // aborted: / superseded:
    }
    message = reason;
}

constexpr size_t kActionStackBytes = 1024 * 1024;   // 1MB（musl 默认才 128KB）

struct ActionThreadArg {
    AppContext* ctx;
    std::string name;
    csrc::Json params;
    bool repeat;      // 循环执行：跑完一轮接着下一轮，直到被停
};

void action_worker(AppContext& ctx, std::string name, csrc::Json params, bool repeat);

void* action_thread_entry(void* ptr) {
    std::unique_ptr<ActionThreadArg> a((ActionThreadArg*)ptr);
    action_worker(*a->ctx, a->name, a->params, a->repeat);
    return nullptr;
}

void action_worker(AppContext& ctx, std::string name, csrc::Json params, bool repeat) {
    const ActionDef* act = nullptr;
    for (const auto& a : list_action_defs()) {
        if (a.id == name) { act = &a; break; }
    }

    RunCtx r;
    r.ctx = &ctx;
    r.t0 = std::chrono::steady_clock::now();
    r.once = !repeat;   // 执行一次才有时限；loop 一直跑，由人停

    // 收尾（无论从哪条路出去都要走）：停电机 + 落状态 + 清理线程句柄
    auto finish = [&](const std::string& reason, const std::string& ret) {
        if (r.moving) ctx.motor_pair->brake();   // 状态机自己忘了停 → 宿主兜底
        std::string state, message;
        classify(reason, ret, state, message);
        if (state == "done") {
            CAM_INFO("[action] %s 正常结束：%s", name.c_str(), message.c_str());
        } else {
            CAM_WARN("[action] %s 结束（%s）：%s", name.c_str(), state.c_str(), message.c_str());
        }
        // 只落状态，**不碰线程对象**：它是"属于下一次运行的资源"，由 script_run 在锁内统一回收。
        std::lock_guard<std::mutex> lk(ctx.script_mu);
        ctx.script_state = state;
        ctx.script_message = message;
        ctx.script_running = false;
    };

    if (!act) {
        finish("failed: 内置动作不存在：" + name, "");
        return;
    }

    // 运行参数 + 必填校验（等价于原 lua 开头那几行 fail）
    RunParams p;
    p.model = params.gets("model");
    p.target = params.getd("target_size", 0);
    p.speed = (int)params.geti("speed", 20);
    p.turn_speed = (int)params.geti("turn_speed", p.speed);
    p.t = load_tuning(params);

    if (p.model.empty()) {
        finish("failed: params.model 必填：这张卡片要用哪个模型找目标", "");
        return;
    }
    if (p.target <= 0) {
        finish("failed: params.target_size 必须是正数（目标框宽，像素）", "");
        return;
    }

    prim_log("开始：模型=%s 目标框宽=%lldpx 直线速度=%d%% 转弯速度=%d%%",
             p.model.c_str(), (long long)p.target, p.speed, p.turn_speed);

    // 一轮 = 跑一遍状态机。loop：跑完接着下一轮直到被停（没有总时长上限）；
    // once：整趟最多 kMaxOnceSeconds，到点由 detect_interrupt 收工。
    std::string ret;
    int round = 0;
    try {
        while (true) {
            round++;
            r.reason.clear();
            r.moving = false;   // 每轮重新算（收尾兜底刹车时用它）
            {
                std::lock_guard<std::mutex> lk(ctx.script_mu);
                ctx.script_round = round;
            }
            if (repeat) {
                CAM_INFO("[action] 跑 %s（第 %d 轮，循环执行）", name.c_str(), round);
            } else {
                CAM_INFO("[action] 跑 %s（执行一次，最多 %d 分钟）", name.c_str(),
                         (int)(kMaxOnceSeconds / 60.0));
            }

            ret = run_action_once(&r, *act, p);

            if (!repeat || ctx.script_abort || ctx.shutdown) {
                // 循环模式是在轮与轮之间被叫停的：这一轮本身正常结束，但状态要标"被停止"
                if (repeat && r.reason.empty()) {
                    r.reason = ctx.shutdown ? "aborted: 服务退出" : "aborted: 收到停止请求";
                }
                break;
            }
            // 轮与轮之间：先把车停住，喘口气再继续 —— 这段时间也让人来得及按停止
            if (r.moving) {
                ctx.motor_pair->brake();
                r.moving = false;
            }
            for (int i = 0; i < 4 && !ctx.script_abort && !ctx.shutdown; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kLoopGapMs / 4));
            }
        }
        finish(r.reason, ret);
    } catch (const Interrupt&) {
        finish(r.reason, ret);
    }
}

}  // namespace

// ────────────────────────── 内置动作注册表 ──────────────────────────

const std::vector<ActionDef>& list_action_defs() {
    static const std::vector<ActionDef> kActions = {
        {"approach", "接近瞄准", false},
        {"grab", "追到就夹", true},
    };
    return kActions;
}

bool is_known_action(const std::string& id) {
    for (const auto& a : list_action_defs()) {
        if (a.id == id) return true;
    }
    return false;
}

// ────────────────────────── 对外接口 ──────────────────────────

csrc::Json script_run(AppContext& ctx, const std::string& name, const csrc::Json& params) {
    csrc::Json j;
    if (!valid_model_name(name)) {   // 同一个"名字会当标识用"的校验（禁 / 与 ..）
        j["ok"] = false;
        j["error"] = "动作名非法（只允许字母数字与 _ - .）：" + name;
        return j;
    }
    // 执行方式：params.mode = "once"（默认，跑一遍就结束，最多 kMaxOnceSeconds）|
    // "loop"（跑完接着跑，直到被停 —— 没有总时长上限）。
    const bool repeat = params.gets("mode") == "loop";

    // 入口就把"内置动作不存在"挡掉：以前是异步失败（先回 ok:true 再变 failed）
    if (!is_known_action(name)) {
        j["ok"] = false;
        j["error"] = "内置动作不存在：" + name;
        return j;
    }
    {
        std::lock_guard<std::mutex> lk(ctx.script_mu);
        if (ctx.script_running) {
            j["ok"] = false;
            j["error"] = "已有脚本在跑（先 POST /api/demo/stop）";
            return j;
        }
        ctx.script_running = true;
        ctx.script_abort = false;
        ctx.script_state = "running";
        ctx.script_message = "启动";
        ctx.script_name = name;
        ctx.script_model = params.gets("model");   // 卡片里的模型（直接跑时是请求里给的）
        ctx.script_card = params.gets("card");     // 卡片名；直接 action+model 跑时为空
        ctx.script_repeat = repeat;
        ctx.script_round = 0;
        ctx.script_calls = 0;
        ctx.script_action.clear();
        ctx.script_notes.clear();
    }
    {
        std::lock_guard<std::mutex> lk(ctx.script_mu);
        // 回收上一次的线程（pthread_join：此刻它一定已跑完，join 立即返回并释放资源）
        if (ctx.script_tid_valid) {
            pthread_join(ctx.script_tid, nullptr);
            ctx.script_tid_valid = false;
        }
        // 创建也放在锁内：否则工作线程可能抢在记账之前跑完，被下一次运行重复回收。
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kActionStackBytes);
        auto* arg = new ActionThreadArg{&ctx, name, params, repeat};
        const int rc = pthread_create(&ctx.script_tid, &attr, action_thread_entry, arg);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            delete arg;
            ctx.script_running = false;
            ctx.script_state = "failed";
            ctx.script_message = "创建动作线程失败（rc=" + std::to_string(rc) + "）";
            j["ok"] = false;
            j["error"] = ctx.script_message;
            return j;
        }
        ctx.script_tid_valid = true;
    }

    j["ok"] = true;
    j["state"] = "running";
    j["script"] = name;
    j["mode"] = repeat ? "loop" : "once";
    return j;
}

csrc::Json script_stop(AppContext& ctx) {
    csrc::Json j;
    {
        std::lock_guard<std::mutex> lk(ctx.script_mu);
        if (!ctx.script_running) {
            j["ok"] = true;
            j["state"] = "idle";
            j["message"] = "没有正在跑的脚本";
            return j;
        }
    }
    ctx.script_abort = true;
    ctx.motor_pair->brake();   // 不等状态机配合，立刻刹车
    CAM_INFO("[action] 收到停止请求");
    j["ok"] = true;
    j["state"] = "aborted";
    return j;
}

csrc::Json script_status(AppContext& ctx) {
    csrc::Json j;
    std::lock_guard<std::mutex> lk(ctx.script_mu);
    j["state"] = ctx.script_state;
    j["script"] = ctx.script_name;       // 动作名（内置动作 id）
    j["mode"] = ctx.script_repeat ? "loop" : "once";
    j["round"] = csrc::Json((int64_t)ctx.script_round);   // 循环执行跑到第几轮
    j["model"] = ctx.script_model;       // 在追哪个模型
    j["card"] = ctx.script_card;         // 哪张卡片（空 = 直接 action+model 跑的）
    j["message"] = ctx.script_message;
    j["calls"] = csrc::Json((int64_t)ctx.script_calls);
    j["action"] = ctx.script_action;
    csrc::Json notes;
    for (const auto& kv : ctx.script_notes) {
        notes[kv.first] = kv.second;
    }
    j["notes"] = notes;
    return j;
}

}  // namespace capp
