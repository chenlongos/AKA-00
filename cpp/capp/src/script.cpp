// capp/script.cpp — Lua 流程脚本宿主
//
// ── 为什么有这一层 ──
// "看 → 对准 → 靠近 → 抓"这类**流程**天生要反复调参（阈值、脉冲时长、速度…）。
// 写在 C++ 里，改一个数就要交叉编译 + 部署 + 重启，一轮几分钟；写成脚本就是改一行
// 存盘重跑。所以：**原语留在 C++（快、稳），流程搬进 demo/*.lua（好改）**。
//
// ── 红线：安全兜底不放进脚本 ──
// 脚本只能拿到「有上限的原语」，下面这些由宿主强制，脚本绕不过去（连 pcall 都不给，
// 所以它也吞不掉宿主的打断）：
//   · 速度上限       drive/forward/... 的参数一律 clamp 到 ±kScriptMaxSpeed
//   · 总时长上限     max_seconds，由看门狗 hook 掐（脚本 while true do end 也掐得住）
//   · 被人的指令取代 有人推进了 motion_seq（摇杆/HTTP 指令）→ 立刻报错退出，交还控制权
//   · stop / 服务退出 / 底盘掉线 —— 同上，立刻退出
//   · 内存上限       自定义分配器给 Lua VM 设预算，脚本狂建 table 也吃不光板子的内存
//   · 脚本卡死       hook 每 N 条指令检查一次打断条件
// 退出时宿主兜底把电机停回静止（脚本自己忘了停也一样）。
//
// 脚本能用的原语清单见 docs/src/05-usage/api.md 的「脚本流程」。

#include "capp/context.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <thread>

// Lua 是 C 库，且它的头文件**不带 extern "C" 守卫** —— 从 C++ 引必须自己包一层，
// 否则符号按 C++ 修饰，链接时全是 "undefined reference to lua_xxx(...)"。
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "csrc/log.hpp"

namespace capp {

namespace {

// 上限类常量（脚本改不了）
// 速度硬上限（%）：脚本/ demo 配置给再大的数，到这里一律砍成 ±这个值。
// 原来定的 35 实测不够用（demo 需要的直线速度在 40~70 之间，填 40/60/100 在车上
// 一模一样 —— 全被砍成 35，看起来就像"配置的速度没生效"），所以放宽到 70。
// 留着它是因为这是唯一拦在"脚本乱发速度"和电机之间的东西，别顺手删。
constexpr int kScriptMaxSpeed = 70;
constexpr int kScriptMinSeconds = 5;
constexpr int kScriptMaxSeconds = 300;
constexpr size_t kScriptMemBytes = 4 * 1024 * 1024;   // 脚本 VM 内存预算
constexpr int kScriptHookEvery = 2000;                // 每多少条 Lua 指令检查一次打断
constexpr int kScriptSleepStepMs = 50;                // sleep_ms 切段睡，便于被中断
// ESP32 固件的"速度命令看门狗"：单发一条只转 0.2~0.9s 就自己停（板上实测；demo 之所以
// 好用，是因为它每一帧都重发 drive()）。所以只要脚本处在"有速度"状态，宿主就替它重发。
constexpr int kScriptKeepaliveMs = 80;

/// 一次运行的全部状态：宿主与各原语共享（原语通过 upvalue 拿到它）
struct RunCtx {
    AppContext* ctx = nullptr;
    std::chrono::steady_clock::time_point t0;
    long long budget_ms = 0;
    int64_t own_seq = 0;    // 本脚本最近一次推进的指令代际号（用来发现"被接管"）
    bool claimed = false;   // 是否动过电机（没动过就不该误判"被接管"）
    bool moving = false;    // 发过速度且还没回静止 → 收尾兜底要停
    int last_left = 0, last_right = 0;   // 当前发给底盘的速度（保活重发用）
    long long last_keepalive_ms = 0;
    std::string reason;     // 打断原因；空 = 正常结束
    size_t mem_used = 0;
};

RunCtx* RC(lua_State* L) { return (RunCtx*)lua_touserdata(L, lua_upvalueindex(1)); }

long long now_ms(const RunCtx* r) {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - r->t0).count();
}

/// 底盘是否真在线。掉线时 AutoReconnectMotorPair 会静默退回 mock（车不动却没错误码），
/// 所以每一拍都得主动查，否则就是"对着空气跑"。
bool motor_ok(AppContext& ctx) { return motor_status_json(ctx).getb("connected", false); }

/// 所有"该打断"的条件集中在这里；命中就 luaL_error（longjmp，脚本接不住）。
/// 顺序有意为之：先看人为的停止，再看超时/接管/硬件。
void check_interrupt(lua_State* L, RunCtx* r) {
    if (!r->reason.empty()) luaL_error(L, "%s", r->reason.c_str());
    if (r->ctx->script_abort) {
        r->reason = "aborted: 收到停止请求";
    } else if (r->ctx->shutdown) {
        r->reason = "aborted: 服务退出";
    } else if (now_ms(r) > r->budget_ms) {
        r->reason = "timeout: 超过 max_seconds（" + std::to_string(r->budget_ms / 1000) + "s）";
    } else if (r->claimed && motion_seq_now(*r->ctx) != r->own_seq) {
        r->reason = "superseded: 被新的运动指令取代（人接管）";
    } else if (!motor_ok(*r->ctx)) {
        r->reason = "motor: 底盘掉线";
    }
    if (!r->reason.empty()) luaL_error(L, "%s", r->reason.c_str());
}

/// 保活：底盘固件要持续收到速度命令才肯转（板测：单发一条只转 0.2~0.9s）。
/// 放在 hook 与 sleep 里各调一次 —— 于是不管脚本在算还是在等，只要还有速度就一直转。
void keepalive_tick(RunCtx* r) {
    if (r->last_left == 0 && r->last_right == 0) return;
    const long long now = now_ms(r);
    if (now - r->last_keepalive_ms < kScriptKeepaliveMs) return;
    r->last_keepalive_ms = now;
    r->ctx->motor_pair->set_speed(r->last_left, r->last_right);
}

/// 看门狗：每 kScriptHookEvery 条指令查一次（脚本死循环、不调用任何原语也跑不掉），
/// 顺带做底盘保活。
void hook_tick(lua_State* L, lua_Debug*) {
    RunCtx* r = (RunCtx*)lua_getextraspace(L);
    if (!r || !r->ctx) return;
    keepalive_tick(r);
    check_interrupt(L, r);
}

/// 声明"这次动作属于本脚本"：取消挂起的定时停 + 推进指令代际号。
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
    const int l = std::max(-kScriptMaxSpeed, std::min(kScriptMaxSpeed, left));
    const int rr = std::max(-kScriptMaxSpeed, std::min(kScriptMaxSpeed, right));
    claim_motion(r);
    r->ctx->motor_pair->set_speed(l, rr);
    r->ctx->collector.set_target_speed(l, rr);
    r->moving = (l != 0 || rr != 0);
    r->last_left = l;
    r->last_right = rr;
    r->last_keepalive_ms = now_ms(r);
    publish(r, action);
}

// ────────────────────────── 原语 ──────────────────────────

int l_forward(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    const int s = (int)luaL_optinteger(L, 1, 20);
    set_speed_checked(r, s, s, "forward");
    return 0;
}
int l_back(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    const int s = (int)luaL_optinteger(L, 1, 20);
    set_speed_checked(r, -s, -s, "back");
    return 0;
}
int l_turn_left(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    const int s = (int)luaL_optinteger(L, 1, 20);
    set_speed_checked(r, -s, s, "turn_left");
    return 0;
}
int l_turn_right(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    const int s = (int)luaL_optinteger(L, 1, 20);
    set_speed_checked(r, s, -s, "turn_right");
    return 0;
}
int l_drive(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    set_speed_checked(r, (int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2), "drive");
    return 0;
}
int l_standby(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    r->ctx->motor_pair->set_speed(0, 0);   // demo 的 MOTOR_STANDBY 也是 drive(0,0)
    r->ctx->collector.set_target_speed(0, 0);
    r->moving = false;
    r->last_left = r->last_right = 0;
    publish(r, "standby");
    return 0;
}
int l_brake(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    r->ctx->motor_pair->brake();
    r->ctx->collector.set_target_speed(0, 0);
    r->moving = false;
    r->last_left = r->last_right = 0;
    publish(r, "brake");
    return 0;
}
/// sleep_ms(ms)：切段睡，每段都查打断 —— 所以 /api/script/stop 不用等脚本醒
int l_sleep_ms(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    long long ms = (long long)luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        check_interrupt(L, r);
        keepalive_tick(r);   // 等待期间不能让底盘固件的看门狗把车停了
        std::this_thread::sleep_for(std::chrono::milliseconds(kScriptSleepStepMs));
    }
    return 0;
}
int l_grab(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    apply_arm_action(*r->ctx, "grab");     // 内部持 arm_mu；ZP10S 是约 3.5s 的整段序列
    publish(r, "grab");
    return 0;
}
int l_release(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    apply_arm_action(*r->ctx, "release");
    publish(r, "release");
    return 0;
}
/// detect(model) → {frame_w=..., boxes={{x1,y1,x2,y2,w,h,cx,cy,area}, ...}}；失败返回 nil, err
int l_detect(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    const char* model = luaL_checkstring(L, 1);
    // 模型名现在多半来自 params().model（外部可控），必须校验 —— 它会拼进
    // demo/models/<名字>.cvimodel。`/api/detect` 有这道校验，Lua 这条路以前没有。
    if (!valid_model_name(model)) {
        lua_pushnil(L);
        lua_pushfstring(L, "模型名非法（只允许字母数字与 _ - .）：%s", model);
        return 2;
    }

    // 可选阈值：detect(model, {conf = 0.6, iou = 0.3})；不给就用默认 0.25 / 0.45。
    // 想按模型分别调又不改脚本，可以从 params() 里读：
    //   local p = params() or {}
    //   detect(model, {conf = p.conf, iou = p.iou})
    double conf = 0, iou = 0;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "conf");
        if (lua_isnumber(L, -1)) conf = lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "iou");
        if (lua_isnumber(L, -1)) iou = lua_tonumber(L, -1);
        lua_pop(L, 1);
    }

    std::vector<csrc::Detection> dets;
    int frame_w = 0;
    std::string err;
    if (!detect_boxes(*r->ctx, model, decode_options(conf, iou), dets, frame_w, err)) {
        // "no frame"（相机刚开、这一拍还没出帧）对视觉伺服来说等价于"这拍没看到目标"，
        // 返回空列表让脚本走它自己的丢帧分支；其余（模型加载失败/相机不可用/填张量失败）
        // 才是硬错误，返回 nil + err 由脚本 fail 掉。
        if (err != "no frame") {
            lua_pushnil(L);
            lua_pushstring(L, err.c_str());
            return 2;
        }
    }
    publish(r, "detect");
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)frame_w);
    lua_setfield(L, -2, "frame_w");
    lua_newtable(L);
    int i = 1;
    for (const auto& d : dets) {
        const float w = d.box.x2 - d.box.x1, h = d.box.y2 - d.box.y1;
        lua_newtable(L);
        lua_pushnumber(L, d.box.x1); lua_setfield(L, -2, "x1");
        lua_pushnumber(L, d.box.y1); lua_setfield(L, -2, "y1");
        lua_pushnumber(L, d.box.x2); lua_setfield(L, -2, "x2");
        lua_pushnumber(L, d.box.y2); lua_setfield(L, -2, "y2");
        lua_pushnumber(L, w);        lua_setfield(L, -2, "w");
        lua_pushnumber(L, h);        lua_setfield(L, -2, "h");
        lua_pushnumber(L, (d.box.x1 + d.box.x2) * 0.5f); lua_setfield(L, -2, "cx");
        lua_pushnumber(L, (d.box.y1 + d.box.y2) * 0.5f); lua_setfield(L, -2, "cy");
        lua_pushnumber(L, w * h);    lua_setfield(L, -2, "area");
        lua_rawseti(L, -2, i++);
    }
    lua_setfield(L, -2, "boxes");
    return 1;
}
int l_elapsed_ms(lua_State* L) {
    RunCtx* r = RC(L);
    check_interrupt(L, r);
    lua_pushinteger(L, (lua_Integer)now_ms(r));
    return 1;
}
int l_motor_connected(lua_State* L) {
    RunCtx* r = RC(L);
    lua_pushboolean(L, motor_ok(*r->ctx) ? 1 : 0);
    return 1;
}
int l_abort_requested(lua_State* L) {
    RunCtx* r = RC(L);
    lua_pushboolean(L, (r->ctx->script_abort || r->ctx->shutdown) ? 1 : 0);
    return 1;
}
/// log(fmt, ...)：写 capp 日志（板上就是 init.sh 的输出），支持 string.format 风格
int l_log(lua_State* L) {
    const int n = lua_gettop(L);
    std::string msg;
    if (n >= 1 && lua_type(L, 1) == LUA_TSTRING) {
        // 有额外参数就按 string.format 走
        if (n > 1) {
            lua_getglobal(L, "string");
            lua_getfield(L, -1, "format");
            lua_remove(L, -2);
            lua_insert(L, 1);
            if (lua_pcall(L, n, 1, 0) == LUA_OK) {
                const char* s = lua_tostring(L, -1);
                msg = s ? s : "";
                lua_pop(L, 1);
            } else {
                lua_pop(L, 1);
                const char* s = lua_tostring(L, 1);
                msg = s ? s : "";
            }
        } else {
            const char* s = lua_tostring(L, 1);
            msg = s ? s : "";
        }
    } else if (n >= 1) {
        const char* s = luaL_tolstring(L, 1, nullptr);
        msg = s ? s : "";
        lua_pop(L, 1);
    }
    CAM_INFO("[script] %s", msg.c_str());
    return 0;
}
/// note(key, value)：发布一个可观测字段给 /api/script/status（脚本自己的调参信息）
int l_note(lua_State* L) {
    RunCtx* r = RC(L);
    const char* k = luaL_checkstring(L, 1);
    const char* v = luaL_tolstring(L, 2, nullptr);
    {
        std::lock_guard<std::mutex> lk(r->ctx->script_mu);
        auto& notes = r->ctx->script_notes;
        bool found = false;
        for (auto& kv : notes) {
            if (kv.first == k) { kv.second = v ? v : ""; found = true; break; }
        }
        if (!found) notes.emplace_back(k, v ? v : "");
    }
    lua_pop(L, 1);
    return 0;
}
/// fail(msg)：脚本主动判定失败 → 宿主把状态报成 failed 并停车
int l_fail(lua_State* L) {
    RunCtx* r = RC(L);
    const char* msg = luaL_checkstring(L, 1);
    r->reason = std::string("failed: ") + msg;
    return luaL_error(L, "%s", r->reason.c_str());
}
/// params() → 启动时传进来的参数表
int l_params(lua_State* L) {
    lua_getglobal(L, "__params");
    return 1;
}

// ────────────────────────── VM 内存预算 ──────────────────────────

void* budget_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    RunCtx* r = (RunCtx*)ud;
    if (nsize == 0) {
        r->mem_used -= osize;
        free(ptr);
        return nullptr;
    }
    const size_t delta = nsize - (ptr ? osize : 0);
    if (r->mem_used + delta > kScriptMemBytes) return nullptr;   // Lua 记作 out of memory
    void* p = realloc(ptr, nsize);
    if (p) r->mem_used += delta;
    return p;
}

// ────────────────────────── csrc::Json → Lua ──────────────────────────

// 注意：非 const —— csrc::Json 的数组下标只有非 const 版本
void json_to_lua(lua_State* L, csrc::Json& j) {
    if (j.is_object()) {
        lua_newtable(L);
        for (auto& kv : j.object()) {
            json_to_lua(L, kv.second);
            lua_setfield(L, -2, kv.first.c_str());
        }
        return;
    }
    if (j.is_array()) {
        lua_newtable(L);
        for (size_t i = 0; i < j.size(); i++) {
            json_to_lua(L, j[i]);
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
        }
        return;
    }
    // 取值一律用 as_* 系列：gets(k)/getb(k) 是"按键查值"，对叶子节点会返回默认值
    // （踩过：params.model 被转成了空字符串，脚本拿着空模型名去 detect，报"注册模型失败
    //   /root/AKA-00/demo/models/.cvimodel"）。
    if (j.is_bool()) { lua_pushboolean(L, j.as_int(0) != 0); return; }
    if (j.is_number()) { lua_pushnumber(L, (lua_Number)j.as_double(0)); return; }
    if (j.is_string()) { lua_pushstring(L, j.as_string().c_str()); return; }
    lua_pushnil(L);
}

// ────────────────────────── 注册与沙箱 ──────────────────────────

struct Reg { const char* name; lua_CFunction fn; };

void register_primitives(lua_State* L, RunCtx* r) {
    const Reg regs[] = {
        {"forward", l_forward},   {"back", l_back},         {"turn_left", l_turn_left},
        {"turn_right", l_turn_right}, {"drive", l_drive},    {"standby", l_standby},
        {"brake", l_brake},       {"sleep_ms", l_sleep_ms},  {"grab", l_grab},
        {"release", l_release},   {"detect", l_detect},      {"elapsed_ms", l_elapsed_ms},
        {"motor_connected", l_motor_connected},
        {"abort_requested", l_abort_requested},
        {"log", l_log},           {"note", l_note},          {"params", l_params},
        {"fail", l_fail},
    };
    for (const auto& reg : regs) {
        lua_pushlightuserdata(L, r);
        lua_pushcclosure(L, reg.fn, 1);   // 每个原语带一个 upvalue = RunCtx*
        lua_setglobal(L, reg.name);
    }
}

/// 只开 base/math/string/table —— **不开** io / os / package / coroutine / debug。
/// 再从 base 里摘掉几样：能碰文件的、能动态加载代码的、以及 pcall/xpcall
/// （留着它们脚本就能把宿主的打断吞掉，安全兜底就形同虚设）。
void open_sandbox(lua_State* L) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);        lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);  lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);  lua_pop(L, 1);
    const char* kill[] = {"dofile", "loadfile", "load", "require", "collectgarbage",
                          "pcall",    "xpcall",   "module"};
    for (const char* k : kill) {
        lua_pushnil(L);
        lua_setglobal(L, k);
    }
    // print → 日志（脚本里 print 也能用，直接进 capp 日志）
    lua_pushlightuserdata(L, (void*)nullptr);
    lua_pushcclosure(L, l_log, 1);   // log 不用 upvalue，给个占位保持签名一致
    lua_setglobal(L, "print");
}

// ────────────────────────── 结果归类 ──────────────────────────

/// reason → (state, message)。空 reason = 脚本正常结束。
void classify(const std::string& reason, const std::string& ret, std::string& state,
              std::string& message) {
    if (reason.empty()) {
        state = "done";
        message = ret.empty() ? "脚本正常结束" : ret;
        return;
    }
    if (reason.rfind("failed:", 0) == 0 || reason.rfind("timeout:", 0) == 0 ||
        reason.rfind("motor:", 0) == 0 || reason.rfind("error:", 0) == 0) {
        state = "failed";
    } else {
        state = "aborted";   // aborted: / superseded:
    }
    message = reason;
}

constexpr size_t kScriptStackBytes = 1024 * 1024;   // 1MB（musl 默认才 128KB，见 context.hpp）

struct ScriptThreadArg {
    AppContext* ctx;
    std::string name;
    csrc::Json params;
    long long budget_ms;
};

void script_worker(AppContext& ctx, std::string name, csrc::Json params, long long budget_ms);

void* script_thread_entry(void* p) {
    std::unique_ptr<ScriptThreadArg> a((ScriptThreadArg*)p);
    script_worker(*a->ctx, a->name, a->params, a->budget_ms);
    return nullptr;
}

void script_worker(AppContext& ctx, std::string name, csrc::Json params, long long budget_ms) {
    RunCtx r;
    r.ctx = &ctx;
    r.t0 = std::chrono::steady_clock::now();
    r.budget_ms = budget_ms;

    // 收尾（无论从哪条路出去都要走）：停电机 + 落状态 + 清理线程句柄
    auto finish = [&](const std::string& reason, const std::string& ret) {
        if (r.moving) ctx.motor_pair->brake();   // 脚本自己忘了停 → 宿主兜底
        std::string state, message;
        classify(reason, ret, state, message);
        // 只落状态，**不碰线程对象**：它是"属于下一次运行的资源"，由 script_run 在锁内
        // 统一回收。以前这里 detach+delete 自己，而 script_run 的 `new std::thread`
        // 赋值又在锁外 —— 两条路可以交错，造成 double free / use-after-free（实测表现是
        // 内存被踩坏后，在完全无关的地方（JPEG 解码的 IDCT）段错误）。
        std::lock_guard<std::mutex> lk(ctx.script_mu);
        ctx.script_state = state;
        ctx.script_message = message;
        ctx.script_running = false;
    };
    auto fail = [&](const std::string& why) {
        CAM_WARN("[script] %s 中止：%s", name.c_str(), why.c_str());
        finish(why, "");
    };

    // 读**动作脚本**（参数是动作名；路径与 demo 列表/接口用同一个函数，别再手拼一遍）
    const std::string path = action_script_path(ctx, name);
    std::string src;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            fail("failed: 脚本打不开 " + path);
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        src = ss.str();
    }
    if (src.empty()) {
        fail("failed: 脚本是空的 " + path);
        return;
    }

    lua_State* L = lua_newstate(budget_alloc, &r);
    if (!L) {
        fail("failed: 创建 Lua VM 失败");
        return;
    }
    open_sandbox(L);
    register_primitives(L, &r);
    json_to_lua(L, params);
    lua_setglobal(L, "__params");
    *(RunCtx**)lua_getextraspace(L) = &r;   // 看门狗 hook 从这里取状态
    lua_sethook(L, hook_tick, LUA_MASKCOUNT, kScriptHookEvery);

    CAM_INFO("[script] 跑 %s（上限 %llds）", path.c_str(), budget_ms / 1000);
    std::string ret;
    if (luaL_loadbuffer(L, src.c_str(), src.size(), ("@" + path).c_str()) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        if (r.reason.empty()) r.reason = std::string("error: 脚本语法错误：") + (e ? e : "?");
    } else if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        if (r.reason.empty()) r.reason = std::string("error: ") + (e ? e : "?");
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        ret = lua_tostring(L, -1);   // 脚本的返回值当结果说明
    } else if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
        r.reason = "failed: 脚本返回 false";
    }
    lua_close(L);
    finish(r.reason, ret);
}

}  // namespace

csrc::Json script_run(AppContext& ctx, const std::string& name, const csrc::Json& params,
                      int max_seconds) {
    csrc::Json j;
    if (!valid_model_name(name)) {   // 同一个"名字会拼进路径"的校验（禁 / 与 ..）
        j["ok"] = false;
        j["error"] = "脚本名非法（只允许字母数字与 _ - .）：" + name;
        return j;
    }
    if (max_seconds < kScriptMinSeconds) max_seconds = kScriptMinSeconds;
    if (max_seconds > kScriptMaxSeconds) max_seconds = kScriptMaxSeconds;

    // 入口就把"动作脚本不存在"挡掉：以前是异步失败（先回 ok:true 再变 failed），
    // 动作名打错一个字母要过一会儿才看得出来
    if (!action_script_exists(ctx, name)) {
        j["ok"] = false;
        j["error"] = "动作脚本不存在：demo/" + name + ".lua";
        return j;
    }
    {
        std::lock_guard<std::mutex> lk(ctx.script_mu);
        if (ctx.script_running) {
            j["ok"] = false;
            j["error"] = "已有脚本在跑（先 POST /api/script/stop）";
            return j;
        }
        ctx.script_running = true;
        ctx.script_abort = false;
        ctx.script_state = "running";
        ctx.script_message = "启动";
        ctx.script_name = name;
        ctx.script_model = params.gets("model");   // 卡片里的模型（直接跑时是请求里给的）
        ctx.script_card = params.gets("card");     // 卡片名；直接 action+model 跑时为空
        ctx.script_calls = 0;
        ctx.script_action.clear();
        ctx.script_notes.clear();
    }
    const long long budget_ms = (long long)max_seconds * 1000;
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
        pthread_attr_setstacksize(&attr, kScriptStackBytes);
        auto* arg = new ScriptThreadArg{&ctx, name, params, budget_ms};
        const int rc = pthread_create(&ctx.script_tid, &attr, script_thread_entry, arg);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            delete arg;
            ctx.script_running = false;
            ctx.script_state = "failed";
            ctx.script_message = "创建脚本线程失败（rc=" + std::to_string(rc) + "）";
            j["ok"] = false;
            j["error"] = ctx.script_message;
            return j;
        }
        ctx.script_tid_valid = true;
    }

    j["ok"] = true;
    j["state"] = "running";
    j["script"] = name;
    j["max_seconds"] = csrc::Json((int64_t)max_seconds);
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
    ctx.motor_pair->brake();   // 不等脚本配合，立刻刹车
    CAM_INFO("[script] 收到停止请求");
    j["ok"] = true;
    j["state"] = "aborted";
    return j;
}

csrc::Json script_status(AppContext& ctx) {
    csrc::Json j;
    std::lock_guard<std::mutex> lk(ctx.script_mu);
    j["state"] = ctx.script_state;
    j["script"] = ctx.script_name;       // 动作名（demo/<动作>.lua）
    j["model"] = ctx.script_model;       // 在追哪个模型
    j["card"] = ctx.script_card;         // 哪张卡片（空 = 直接 action+model 跑的）
    j["message"] = ctx.script_message;
    j["calls"] = csrc::Json((int64_t)ctx.script_calls);
    j["action"] = ctx.script_action;
    csrc::Json notes;
    for (const auto& kv : ctx.script_notes) {
        // note() 传进来的都是字符串（脚本自己格式化），免去猜类型
        notes[kv.first] = kv.second;
    }
    j["notes"] = notes;
    return j;
}

}  // namespace capp
