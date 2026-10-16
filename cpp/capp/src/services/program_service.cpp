#include "capp/program_service.hpp"
#include "capp/context.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <mbedtls/sha256.h>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace capp {
namespace {
using csrc::Json;
using Clock = std::chrono::steady_clock;
double now_ms() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }
Json failure(const std::string& error) { Json j; j["ok"] = false; j["error"] = error; return j; }
const std::set<std::string> capabilities_v1 = {
    "forward", "backward", "turnAngle", "forwardSeconds", "backwardSeconds", "turn",
    "grab", "release", "wait", "print", "stopped", "highlightBlock", "checkpoint", "yield",
    "loopTick", "loopYield", "loopDone", "emptyLoop", "loopLimit", "repeatLimit", "loopInputError",
    "procedureEnter", "procedureExit", "dataGet", "listItem", "requireList", "dataLength", "jsonText"
};
std::string digest(const std::string& value) {
    unsigned char bytes[32];
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes, 0))
        throw std::runtime_error("无法计算程序摘要");
    std::string result;
    for (unsigned char b : bytes) { result += "0123456789abcdef"[b >> 4]; result += "0123456789abcdef"[b & 15]; }
    return result;
}
bool read_text(const std::string& path, std::string& text, size_t limit = 512 * 1024) {
    std::ifstream file(path, std::ios::binary);
    char bytes[4096];
    while (file.read(bytes, sizeof bytes) || file.gcount()) {
        text.append(bytes, file.gcount());
        if (text.size() > limit) return false;
    }
    return file.eof() && !file.bad();
}
bool write_text(const std::string& path, const std::string& text, bool durable) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t written = 0;
    while (written < text.size()) {
        ssize_t n = write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        written += static_cast<size_t>(n);
    }
    bool ok = !durable || fsync(fd) == 0;
    if (close(fd)) ok = false;
    return ok;
}
void sync_directory(const std::string& directory, bool durable) {
    if (!durable) return;
    int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("无法打开程序目录");
    const int rc = fsync(fd);
    close(fd);
    if (rc) throw std::runtime_error("无法同步程序目录");
}
void validate_package(Json& bundle) {
    if (!bundle.is_object() || !bundle.get("formatVersion") || !bundle.get("sdkVersion") ||
        !bundle.get("formatVersion")->is_number() || !bundle.get("sdkVersion")->is_number() ||
        bundle.getd("formatVersion") != 1 || bundle.getd("sdkVersion") != 1)
        throw std::runtime_error("不支持的程序包版本");
    const auto* program = bundle.get("program");
    const auto* required = bundle.get("requiredCapabilities");
    const auto* settings = bundle.get("settings");
    if (!program || !program->is_string() || program->as_string().empty() || program->as_string().size() > 128 * 1024)
        throw std::runtime_error("程序内容为空或超过 128 KiB");
    if (!required || !required->is_array() || required->size() > 64) throw std::runtime_error("缺少程序能力清单");
    for (const auto& capability : required->array())
        if (!capability.is_string() || !capabilities_v1.count(capability.as_string()))
            throw std::runtime_error("板端不支持能力: " + capability.as_string());
    if (!settings || !settings->is_object()) throw std::runtime_error("缺少运行配置");
    for (const auto& field : {"moveSpeed", "turnSpeed", "maxRuntimeMs"}) {
        const Json* value = settings->get(field);
        const double number = value ? value->as_double(-1) : -1;
        const bool runtime = std::string(field) == "maxRuntimeMs";
        if (!value || !value->is_number() || !std::isfinite(number) || number < (runtime ? 100 : 0) ||
            number > (runtime ? 600000 : 50) || std::floor(number) != number)
            throw std::runtime_error("运行配置无效: " + std::string(field));
    }
    bundle["name"] = bundle.gets("name", "Blockly 程序").substr(0, 120);
    if (bundle.dump().size() > 512 * 1024) throw std::runtime_error("程序包超过 512 KiB");
}
struct Child { pid_t pid = -1; int input = -1; int output = -1; };
Child spawn_worker(const std::string& runner, const std::string& package,
                   const std::string& sdk, bool check) {
    int input[2], output[2];
    if (pipe(input)) throw std::runtime_error("无法创建运行器输入管道");
    if (pipe(output)) { close(input[0]); close(input[1]); throw std::runtime_error("无法创建运行器输出管道"); }
    for (int fd : {input[0], input[1], output[0], output[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    for (int fd : {input[0], input[1], output[0], output[1]}) posix_spawn_file_actions_addclose(&actions, fd);
    std::vector<std::string> args{runner};
    if (check) args.push_back("--check");
    args.push_back(package); args.push_back(sdk);
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(&arg[0]);
    argv.push_back(nullptr);
    Child child;
    const int rc = posix_spawn(&child.pid, runner.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(input[0]); close(output[1]);
    if (rc) {
        close(input[1]); close(output[0]);
        throw std::runtime_error("无法启动 Blockly 运行器: " + std::string(std::strerror(rc)));
    }
    child.input = input[1]; child.output = output[0];
    return child;
}
void close_child(Child& child) {
    if (child.input >= 0) close(child.input);
    if (child.output >= 0) close(child.output);
    child.input = child.output = -1;
}
void terminate_child(Child& child) {
    if (child.pid < 0) return;
    kill(child.pid, SIGTERM);
    int status;
    for (int i = 0; i < 15; ++i) {
        const pid_t result = waitpid(child.pid, &status, WNOHANG);
        if (result == child.pid || (result < 0 && errno == ECHILD)) { child.pid = -1; return; }
        usleep(20000);
    }
    kill(child.pid, SIGKILL);
    while (waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {}
    child.pid = -1;
}
bool wait_child_success(Child& child) {
    const double until = now_ms() + 1000;
    while (now_ms() < until) {
        int status = 0;
        const pid_t result = waitpid(child.pid, &status, WNOHANG);
        if (result == child.pid) {
            child.pid = -1;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0 && errno != EINTR) return false;
        usleep(10000);
    }
    return false;
}
bool write_response(int fd, const std::string& text) {
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = write(fd, text.data() + written, text.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}
std::string syntax_check(const std::string& runner, const std::string& path, const std::string& sdk) {
    Child child = spawn_worker(runner, path, sdk, true);
    std::string text;
    int exit_status = 0;
    bool finished = false;
    const double until = now_ms() + 5000;
    while (now_ms() < until) {
        pollfd fd{child.output, POLLIN, 0};
        if (poll(&fd, 1, 20) > 0) {
            char bytes[4096]; ssize_t size = read(child.output, bytes, sizeof bytes);
            if (size > 0) text.append(bytes, size);
        }
        if (text.size() > 16384) break;
        if (waitpid(child.pid, &exit_status, WNOHANG) == child.pid) { finished = true; child.pid = -1; break; }
    }
    terminate_child(child); close_child(child);
    if (finished && WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 0) return "";
    Json error;
    if (Json::parse(text, error) && !error.gets("error").empty()) return error.gets("error");
    return finished ? "程序语法检查失败或运行器无法读取 SDK" : "程序语法检查超时";
}
double argument(const Json& args, size_t index, double minimum, double maximum) {
    if (!args.is_array() || index >= args.size() || !args.array()[index].is_number())
        throw std::runtime_error("动作参数必须是数字");
    const double value = args.array()[index].as_double();
    if (!std::isfinite(value) || value < minimum || value > maximum) throw std::runtime_error("动作参数超出范围");
    return value;
}
}  // namespace

struct ProgramService::Impl {
    AppContext& ctx;
    std::string directory, runner, sdk;
    bool durable;
    mutable std::mutex mu;
    std::mutex admin;
    std::thread worker;
    std::thread autorun_worker;
    std::atomic<bool> closing{false};
    bool autorun_initialized = false;
    std::string boot_marker = "/tmp/aka-blockly-autorun.claim";
    csrc::BlocklyConfig autorun_config;
    std::string autorun_state, autorun_message;
    int64_t autorun_motion_seq = 0;
    std::atomic<bool> abort{false};
    Json package;
    std::string program_id, run_id, state = "empty", message, block_id;
    std::deque<Json> entries;
    int64_t next_log = 0;
    std::atomic<int64_t> own_seq{-1};
    double started = 0, elapsed = 0, deadline = 0;
    std::atomic<bool> touched_motor{false};
    bool stop_confirmed = true;
    unsigned counter = 0;

    explicit Impl(AppContext& c) : ctx(c), autorun_config(c.config.blockly),
        autorun_state(autorun_config.autorun_enabled ? "not_scheduled" : "disabled") {
        const char* storage = getenv("AKA_PROGRAM_DIR");
        const char* executable = getenv("AKA_BLOCKLY_RUNNER");
        directory = storage ? storage : ctx.app_dir + "/programs";
        runner = executable ? executable : ctx.app_dir + "/aka-blockly-runner";
        sdk = ctx.app_dir + "/blockly/robot-sdk.js";
        const char* volatile_mode = getenv("AKA_PROGRAM_VOLATILE");
        durable = !volatile_mode || std::string(volatile_mode) != "1";
        std::string text;
        try {
            if (read_text(directory + "/current.json", text)) {
                if (!Json::parse(text, package)) throw std::runtime_error("程序文件 JSON 无效");
                validate_package(package);
                program_id = digest(package.dump()); state = "ready";
                // A deployment stores its boot selection in the same atomic file as
                // its code. It does not require a second config-file transaction.
                if (package.getb("autorunOnBoot")) {
                    autorun_config.autorun_enabled = true;
                    autorun_config.autorun_program_id = program_id;
                    autorun_state = "not_scheduled";
                }
            }
        } catch (const std::exception& e) { package = Json(); state = "failed"; message = e.what(); }
        text.clear(); Json previous;
        if (!program_id.empty() && read_text(directory + "/last-run.json", text) && Json::parse(text, previous) &&
            previous.gets("programId") == program_id &&
            (previous.gets("state") == "running" || previous.gets("state") == "stopping")) {
            state = "interrupted"; message = "上次运行被服务重启中断；等待重新启动";
        }
    }
    Json snapshot() const {
        std::lock_guard<std::mutex> lock(mu);
        Json result;
        result["ok"] = true; result["state"] = state;
        result["programId"] = program_id; result["runId"] = run_id;
        result["name"] = package.gets("name"); result["error"] = message;
        result["currentBlockId"] = block_id;
        result["elapsedMs"] = ctx.program_running ? std::max(0.0, now_ms() - started) : elapsed;
        result["stopConfirmed"] = stop_confirmed;
        { std::lock_guard<std::mutex> arm(ctx.arm_mu); result["armBusy"] = ctx.arm_busy; }
        result["storage"] = durable ? "persistent" : "volatile";
        result["autorun"]["enabled"] = autorun_config.autorun_enabled;
        result["autorun"]["programId"] = autorun_config.autorun_program_id;
        result["autorun"]["state"] = autorun_state;
        result["autorun"]["message"] = autorun_message;
        return result;
    }
    void autorun_result(const std::string& value, const std::string& reason) {
        std::lock_guard<std::mutex> lock(mu);
        autorun_state = value; autorun_message = reason;
    }
    bool autorun_pending() const {
        std::lock_guard<std::mutex> lock(mu);
        return autorun_state == "waiting";
    }
    void cancel_autorun(const std::string& reason) {
        std::lock_guard<std::mutex> lock(mu);
        if (autorun_state == "waiting") {
            autorun_state = "cancelled"; autorun_message = reason;
        }
    }
    void log(const std::string& type, const std::string& text, const Json& value = Json()) {
        std::lock_guard<std::mutex> lock(mu);
        Json entry;
        entry["seq"] = ++next_log; entry["runId"] = run_id; entry["type"] = type;
        entry["atMs"] = std::max(0.0, now_ms() - started); entry["message"] = text;
        if (!value.is_null()) entry["value"] = value;
        entries.push_back(entry);
        if (entries.size() > 256) entries.pop_front();
    }
    bool owns() const { return motion_seq_now(ctx) == own_seq.load(); }
    void check() const {
        if (abort || ctx.shutdown || closing) throw std::runtime_error("程序已停止");
        if (now_ms() >= deadline) throw std::runtime_error("程序执行超过时限");
        if (!owns()) throw std::runtime_error("控制权已被手动指令接管");
    }
    void delay(double ms) {
        const double until = now_ms() + ms;
        do { check(); if (now_ms() >= until) break; usleep(10000); } while (true);
    }
    void begin_motion(const std::function<void()>& action) {
        check();
        if (!motor_status_json(ctx).getb("connected")) throw std::runtime_error("底盘未连接");
        if (!cancel_pending_stop_if_owned(ctx, own_seq.load())) throw std::runtime_error("控制权已被接管");
        std::lock_guard<std::mutex> lock(ctx.timer_mu);
        if (abort || ctx.shutdown || ctx.motion_seq != own_seq.load()) throw std::runtime_error("动作已取消");
        own_seq = ++ctx.motion_seq;
        touched_motor = true;
        action();
    }
    void brake_if_owned() {
        if (!touched_motor) return;
        std::lock_guard<std::mutex> lock(ctx.timer_mu);
        if (ctx.motion_seq != own_seq.load()) return;
        own_seq = ++ctx.motion_seq;
        ctx.motor_pair->brake();
        ctx.collector.set_target_speed(0, 0);
    }
    bool stationary() {
        if (!motor_status_json(ctx).getb("connected")) return false;
        // Read actual motor feedback rather than a possibly stale collector snapshot.
        // Require repeated zero readings; a successful write alone is not a stop ACK.
        for (int i = 0; i < 3; ++i) {
            if (!ctx.motor_pair->ping()) return false;
            int left = 0, right = 0;
            ctx.motor_pair->get_speeds(left, right);
            if (left || right) return false;
            if (i != 2) usleep(60000);
        }
        return true;
    }
    void timed_motion(const std::string& action, int speed, double seconds) {
        begin_motion([&] { apply_base_action(ctx, action, speed); });
        delay(seconds * 1000);
        brake_if_owned();
    }
    void distance_motion(int direction, double value, int speed) {
        if (ctx.motor_pair->move_state() < 0) throw std::runtime_error("底盘没有距离动作反馈");
        int baseline = 0;
        begin_motion([&] {
            baseline = ctx.motor_pair->move_state();
            ctx.motor_pair->move_distance(static_cast<uint8_t>(direction), static_cast<uint8_t>(speed),
                                          static_cast<int32_t>(std::llround(value * 10)));
        });
        const double until = now_ms() + 30000;
        bool fresh = baseline != 2;
        while (now_ms() < until) {
            check();
            const int status = ctx.motor_pair->move_state();
            if (status < 0) throw std::runtime_error("距离动作反馈中断");
            if (status != baseline || status == 1) fresh = true;
            if (status == 3 && (fresh || baseline != 3)) throw std::runtime_error("底盘中止了距离动作");
            if (status == 2 && fresh) { brake_if_owned(); return; }
            usleep(20000);
        }
        throw std::runtime_error("距离或角度动作 30 秒内未完成");
    }
    Json invoke(const std::string& method, const Json& args, const Json& settings) {
        check();
        if (!args.is_array() || args.size() > 3) throw std::runtime_error("动作参数无效");
        if (method == "stopped") return Json(false);
        if (method == "checkpoint") return Json();
        if (method == "yield") { delay(16); return Json(); }
        if (method == "highlightBlock") {
            if (args.size() != 1 || !args.array()[0].is_string()) throw std::runtime_error("积木编号无效");
            std::lock_guard<std::mutex> lock(mu); block_id = args.array()[0].as_string().substr(0, 128); return Json();
        }
        if (method == "print") {
            if (args.size() != 1) throw std::runtime_error("输出参数无效");
            log("print", args.array()[0].as_string().substr(0, 2048)); return Json();
        }
        if (method == "wait") { delay(argument(args, 0, 0, 60) * 1000); return Json(); }
        const int speed = static_cast<int>(settings.getd(
            method == "turn" || method == "turnAngle" ? "turnSpeed" : "moveSpeed", 30));
        log("action", method, args);
        if (method == "forward" || method == "backward") {
            const double cm = argument(args, 0, 0.1, 500);
            if (speed > 0) distance_motion(method == "forward" ? 0 : 1, cm, speed);
        } else if (method == "turnAngle") {
            const std::string direction = args.array().empty() ? "" : args.array()[0].as_string();
            if (direction != "left" && direction != "right") throw std::runtime_error("转向无效");
            const double degrees = argument(args, 1, 1, 360);
            if (speed > 0) distance_motion(direction == "left" ? 2 : 3, degrees, speed);
        } else if (method == "forwardSeconds" || method == "backwardSeconds") {
            const double seconds = argument(args, 0, 0.1, 30);
            if (speed > 0) timed_motion(method == "forwardSeconds" ? "up" : "down", speed, seconds);
        } else if (method == "turn") {
            const std::string direction = args.array().empty() ? "" : args.array()[0].as_string();
            if (direction != "left" && direction != "right") throw std::runtime_error("转向无效");
            const double seconds = argument(args, 1, 0.1, 30);
            if (speed > 0) timed_motion(direction, speed, seconds);
        } else if (method == "grab" || method == "release") {
            {
                std::lock_guard<std::mutex> lock(ctx.timer_mu);
                if (abort || ctx.shutdown || ctx.motion_seq != own_seq.load()) throw std::runtime_error("夹爪动作已取消");
                { std::lock_guard<std::mutex> arm(ctx.arm_mu);
                  if (ctx.arm_busy) throw std::runtime_error("夹爪正忙"); }
                if (!ctx.gripper || !ctx.gripper->ping()) throw std::runtime_error("夹爪无通信反馈，请检查 UART2 和舵机供电");
                if (abort || ctx.shutdown || ctx.motion_seq != own_seq.load()) throw std::runtime_error("夹爪动作已取消");
                if (apply_arm_action(ctx, method) != ArmResult::Accepted) throw std::runtime_error("夹爪正忙");
            }
            // Existing arm sequences finish their calibrated trajectory; cancellation blocks subsequent actions.
            const double until = now_ms() + 15000;
            for (;;) {
                std::unique_lock<std::mutex> lock(ctx.arm_mu);
                if (!ctx.arm_busy) { if (!ctx.arm_error.empty()) throw std::runtime_error(ctx.arm_error); break; }
                if (now_ms() >= until) throw std::runtime_error("夹爪动作超时");
                ctx.arm_done.wait_for(lock, std::chrono::milliseconds(20));
            }
            check();
        } else {
            throw std::runtime_error("板端不支持 robot." + method);
        }
        log("action-completed", method);
        return Json();
    }
    void persist_state(const std::string& value) {
        Json j; j["state"] = value; j["runId"] = run_id; j["programId"] = program_id;
        const std::string temporary = directory + "/last-run.json.new";
        if (!write_text(temporary, j.dump(), durable) || rename(temporary.c_str(), (directory + "/last-run.json").c_str()))
            throw std::runtime_error("无法保存程序运行状态");
        sync_directory(directory, durable);
    }
    void execute(Json settings) {
        Child child;
        std::string error;
        bool completed = false;
        try {
            check();
            child = spawn_worker(runner, directory + "/current.json", sdk, false);
            std::string pending;
            while (!completed) {
                check();
                pollfd fd{child.output, POLLIN, 0};
                if (poll(&fd, 1, 20) <= 0) continue;
                char bytes[2048];
                const ssize_t count = read(child.output, bytes, sizeof bytes);
                if (count <= 0) throw std::runtime_error("Blockly 运行器提前退出");
                pending.append(bytes, count);
                if (pending.size() > 32768) throw std::runtime_error("Blockly 运行器消息过大");
                size_t newline;
                while ((newline = pending.find('\n')) != std::string::npos) {
                    Json request;
                    const std::string line = pending.substr(0, newline); pending.erase(0, newline + 1);
                    if (!Json::parse(line, request)) throw std::runtime_error("Blockly 运行器消息无效");
                    if (request.gets("type") == "done") { completed = true; break; }
                    if (request.gets("type") == "error") throw std::runtime_error(request.gets("error", "JavaScript 执行失败"));
                    const Json* args = request.get("args");
                    if (request.gets("type") != "call" || !args) throw std::runtime_error("未知运行器请求");
                    Json response;
                    try { response["value"] = invoke(request.gets("method"), *args, settings); response["ok"] = true; }
                    catch (const std::exception& e) { response = failure(e.what()); }
                    const std::string text = response.dump() + "\n";
                    if (!write_response(child.input, text))
                        throw std::runtime_error("Blockly 运行器连接已关闭");
                    if (!response.getb("ok")) throw std::runtime_error(response.gets("error"));
                }
            }
            if (!wait_child_success(child)) throw std::runtime_error("Blockly 运行器未正常退出");
        } catch (const std::exception& e) { error = e.what(); }
        terminate_child(child); close_child(child);
        try {
            brake_if_owned();
            if (touched_motor && owns()) {
                bool confirmed = false;
                const double until = now_ms() + 1200;
                while (now_ms() < until && owns()) {
                    if (stationary() && owns()) {
                        confirmed = true; break;
                    }
                    usleep(20000);
                }
                std::lock_guard<std::mutex> lock(mu);
                stop_confirmed = confirmed;
                if (!confirmed && error.empty()) error = "未取得底盘停车确认";
            }
        } catch (const std::exception& e) { if (error.empty()) error = e.what(); }
        std::string final_state = abort ? "stopped" : error.empty() && completed ? "completed" : "failed";
        try { persist_state(final_state); } catch (const std::exception& e) { error = e.what(); final_state = "failed"; }
        log(final_state, error);
        {
            std::lock_guard<std::mutex> lock(mu);
            elapsed = now_ms() - started;
            state = final_state; message = error;
            ctx.program_running = false;
        }
    }
};

ProgramService::ProgramService(AppContext& ctx) : impl_(new Impl(ctx)) {}
ProgramService::~ProgramService() { shutdown(); }
Json ProgramService::capabilities() const {
    Json result; result["ok"] = true; result["formatVersion"] = 1; result["sdkVersion"] = 1;
    result["maxPackageBytes"] = 512 * 1024; result["maxProgramBytes"] = 128 * 1024;
    result["capabilities"] = Json(Json::Type::Array);
    for (const auto& method : capabilities_v1) result["capabilities"].push_back(method);
    result["runnerAvailable"] = access(impl_->runner.c_str(), X_OK) == 0 && access(impl_->sdk.c_str(), R_OK) == 0;
    result["deployAvailable"] = impl_->durable;
    return result;
}
Json ProgramService::upload(const Json& input, int& http_status) {
    auto& p = *impl_;
    std::lock_guard<std::mutex> admin(p.admin);
    return save_package(input, http_status, false);
}
Json ProgramService::save_package(const Json& input, int& http_status, bool autorun) {
    auto& p = *impl_;
    if (p.closing || p.ctx.shutdown) { http_status = 409; return failure("服务正在退出"); }
    if (p.ctx.program_running) { http_status = 409; return failure("请先停止板端程序再下载"); }
    if (p.worker.joinable()) p.worker.join();
    Json bundle = input;
    // Only /deploy may arm a program for boot; plain uploads never inherit it.
    bundle.erase("autorunOnBoot");
    if (autorun) bundle["autorunOnBoot"] = true;
    bool committed = false;
    try {
        validate_package(bundle);
        p.cancel_autorun("下载程序已取消本次开机运行");
        const std::string text = bundle.dump();
        if (text.size() > 512 * 1024) throw std::runtime_error("程序包超过 512 KiB");
        if (mkdir(p.directory.c_str(), 0700) && errno != EEXIST) throw std::runtime_error("无法创建程序目录");
        const std::string candidate = p.directory + "/upload.json";
        if (!write_text(candidate, text, p.durable)) throw std::runtime_error("程序文件写入失败");
        std::string error;
        try { error = syntax_check(p.runner, candidate, p.sdk); }
        catch (...) { unlink(candidate.c_str()); throw; }
        if (!error.empty()) { unlink(candidate.c_str()); throw std::runtime_error(error); }
        const std::string id = digest(text);
        if (rename(candidate.c_str(), (p.directory + "/current.json").c_str())) throw std::runtime_error("程序文件提交失败");
        committed = true;
        sync_directory(p.directory, p.durable);
        {
            std::lock_guard<std::mutex> lock(p.mu);
            p.package = bundle; p.program_id = id; p.state = "ready"; p.message.clear();
            p.run_id.clear(); p.block_id.clear(); p.elapsed = 0;
            p.autorun_config = p.ctx.config.blockly;
            if (autorun) {
                p.autorun_config.autorun_enabled = true;
                p.autorun_config.autorun_program_id = id;
                p.autorun_state = "armed";
                p.autorun_message = "已部署，本次立即运行，下次开机自动运行";
            } else if (!p.autorun_config.autorun_enabled) {
                p.autorun_state = "disabled";
                p.autorun_message.clear();
            }
        }
        http_status = 200; return p.snapshot();
    } catch (const std::exception& e) {
        if (committed) {
            // A failed directory flush must never leave an old in-memory ID pointing
            // at new on-disk source. Require a new upload or service reload.
            std::lock_guard<std::mutex> lock(p.mu);
            p.program_id.clear(); p.package = Json(); p.state = "failed"; p.message = e.what();
        }
        http_status = 400; return failure(e.what());
    }
}
Json ProgramService::deploy(const Json& input, int& http_status) {
    auto& p = *impl_;
    std::lock_guard<std::mutex> admin(p.admin);
    std::lock_guard<std::mutex> autonomy(p.ctx.autonomy_mu);
    if (!p.durable) { http_status = 409; return failure("当前程序目录为临时存储，不能部署开机程序"); }
    if (p.ctx.config.blockly.startup_timeout_ms < 100 || p.ctx.config.blockly.startup_timeout_ms > 600000) {
        http_status = 400; return failure("板端开机等待时限配置无效，请先修正 startup_timeout_ms");
    }
    if (p.ctx.program_running || p.ctx.shutdown || p.closing) {
        http_status = 409; return failure("请先停止板端程序，或等待服务就绪");
    }
    { std::lock_guard<std::mutex> demo(p.ctx.script_mu);
      if (p.ctx.script_running) { http_status = 409; return failure("请先停止 Demo 再部署"); } }
    { std::lock_guard<std::mutex> arm(p.ctx.arm_mu);
      if (p.ctx.arm_busy) { http_status = 409; return failure("请等待机械臂动作完成再部署"); } }
    const int64_t motion_seq = motion_seq_now(p.ctx);
    if (p.ctx.motor_pair->move_state() == 1 || !p.stationary()) {
        http_status = 409; return failure("请确认底盘已连接并静止后再部署");
    }
    // Deploy starts the program explicitly. Consume this boot's automatic attempt
    // BEFORE publishing it, so a service crash/restart cannot start it a second time.
    if (mkdir(p.boot_marker.c_str(), 0700) && errno != EEXIST) {
        http_status = 500; return failure("无法登记本次开机，部署未执行");
    }
    Json saved = save_package(input, http_status, true);
    if (http_status != 200) return saved;
    const Json started = run_locked(saved.gets("programId"), http_status, false, motion_seq);
    Json result = p.snapshot();
    result["deployed"] = true;
    if (http_status != 202) { result["ok"] = false; result["error"] = started.gets("error"); }
    return result;
}
Json ProgramService::run(const std::string& program_id, int& http_status) {
    return run_internal(program_id, http_status, false);
}
Json ProgramService::run_internal(const std::string& program_id, int& http_status, bool automatic) {
    auto& p = *impl_;
    std::lock_guard<std::mutex> admin(p.admin);
    std::lock_guard<std::mutex> autonomy(p.ctx.autonomy_mu);
    return run_locked(program_id, http_status, automatic);
}
Json ProgramService::run_locked(const std::string& program_id, int& http_status, bool automatic,
                                int64_t expected_motion_seq) {
    auto& p = *impl_;
    if (p.ctx.program_running || p.ctx.shutdown || p.closing) { http_status = 409; return failure("已有程序在运行或服务正在退出"); }
    if (automatic && !p.autorun_pending()) { http_status = 409; return failure("开机运行已取消"); }
    { std::lock_guard<std::mutex> lock(p.ctx.script_mu);
      if (p.ctx.script_running) { http_status = 409; return failure("请先停止 Demo 再运行 Blockly 程序"); } }
    if (p.worker.joinable()) p.worker.join();
    if (program_id.empty() || program_id != p.program_id) { http_status = 409; return failure("板端程序版本已改变，请重新下载或读取状态"); }
    if (!p.stop_confirmed) {
        if (!p.stationary()) { http_status = 409; return failure("上次停车未确认，请检查底盘状态"); }
        std::lock_guard<std::mutex> lock(p.mu);
        p.stop_confirmed = true;
    }
    {
        // Compare and reserve in the same critical section as manual controls.
        // A later manual control still cancels the program before its next effect.
        std::lock_guard<std::mutex> motion(p.ctx.timer_mu);
        if (automatic && p.ctx.motion_seq != p.autorun_motion_seq) {
            p.autorun_result("cancelled", "人工控制已取消本次开机运行");
            http_status = 409; return failure("人工控制已取消本次开机运行");
        }
        if (expected_motion_seq >= 0 && p.ctx.motion_seq != expected_motion_seq) {
            http_status = 409; return failure("部署期间收到人工控制，程序已保存但未启动");
        }
        p.own_seq = p.ctx.motion_seq;
    }
    if (!automatic) p.cancel_autorun("手动启动已取消本次开机运行");
    p.abort = false;
    p.touched_motor = false;
    {
        std::lock_guard<std::mutex> lock(p.mu);
        p.started = now_ms();
        p.deadline = p.started + p.package.get("settings")->getd("maxRuntimeMs");
        p.run_id = std::to_string(getpid()) + "-" + std::to_string(++p.counter) + "-" + std::to_string(static_cast<int64_t>(p.started));
        p.state = "running"; p.message.clear(); p.block_id.clear(); p.elapsed = 0;
        p.entries.clear(); p.next_log = 0;
    }
    try {
        p.persist_state("running");
        p.ctx.program_running = true;
        p.log("running", "板端程序已启动");
        Json settings = *p.package.get("settings");
        p.worker = std::thread([&p, settings] { p.execute(settings); });
    } catch (const std::exception& e) {
        p.ctx.program_running = false;
        { std::lock_guard<std::mutex> lock(p.mu); p.state = "failed"; p.message = e.what(); }
        http_status = 500; return failure(e.what());
    }
    http_status = 202; return p.snapshot();
}
Json ProgramService::stop(const std::string& run_id, int& http_status) {
    auto& p = *impl_;
    std::lock_guard<std::mutex> admin(p.admin);
    std::lock_guard<std::mutex> autonomy(p.ctx.autonomy_mu);
    bool active = false;
    bool cancelled_pending = false;
    { std::lock_guard<std::mutex> lock(p.mu);
      // Before a runId exists, an empty ID may cancel only this pending boot attempt.
      // Existing and stale nonempty run IDs retain the normal ownership checks.
      if (run_id.empty() && p.run_id.empty() && p.autorun_state == "waiting" && !p.ctx.program_running) {
          p.autorun_state = "cancelled";
          p.autorun_message = "停止请求已取消本次开机运行";
          cancelled_pending = true;
      } else {
          if (run_id.empty() || run_id != p.run_id) { http_status = 409; return failure("运行编号不匹配，请刷新板端状态"); }
          active = p.ctx.program_running;
          if (active) { p.abort = true; p.state = "stopping"; }
      } }
    if (cancelled_pending) { http_status = 200; return p.snapshot(); }
    try { if (active) p.brake_if_owned(); }
    catch (const std::exception& e) { p.log("stop-error", e.what()); }
    http_status = p.ctx.program_running ? 202 : 200;
    return p.snapshot();
}
Json ProgramService::status() const { return impl_->snapshot(); }
Json ProgramService::logs(const std::string& run_id, int64_t after) const {
    auto& p = *impl_;
    std::lock_guard<std::mutex> lock(p.mu);
    Json result; result["ok"] = true; result["runId"] = p.run_id;
    result["entries"] = Json(Json::Type::Array);
    result["truncated"] = !p.entries.empty() && after + 1 < p.entries.front().geti("seq");
    if (run_id.empty() || run_id == p.run_id)
        for (const auto& entry : p.entries) if (entry.geti("seq") > after) result["entries"].push_back(entry);
    result["nextSeq"] = p.next_log; return result;
}
void ProgramService::start_autorun(const std::string& boot_marker) {
    auto& p = *impl_;
    std::lock_guard<std::mutex> admin(p.admin);
    if (p.autorun_initialized || p.closing) return;
    p.autorun_initialized = true;
    p.boot_marker = boot_marker;
    const auto cfg = p.autorun_config;
    if (!cfg.autorun_enabled) return;

    // Claim BEFORE validation/readiness/execution. Any failure, manual cancellation,
    // crash or later service restart consumes this boot's only automatic attempt.
    if (mkdir(boot_marker.c_str(), 0700)) {
        p.autorun_result(errno == EEXIST ? "skipped" : "failed",
            errno == EEXIST ? "本次开机已登记过自动运行，不再重复" : "无法创建开机运行标记");
        return;
    }
    if (cfg.autorun_program_id.size() != 64 ||
        cfg.autorun_program_id.find_first_not_of("0123456789abcdef") != std::string::npos ||
        cfg.startup_timeout_ms < 100 || cfg.startup_timeout_ms > 600000) {
        p.autorun_result("failed", "开机运行配置无效，需要程序 SHA-256 和 100～600000 ms 等待时限");
        return;
    }
    if (p.program_id != cfg.autorun_program_id ||
        (p.state != "ready" && !(p.package.getb("autorunOnBoot") && p.state == "interrupted"))) {
        p.autorun_result("skipped", "保存的程序版本不匹配或上次执行被中断，等待人工启动");
        return;
    }
    if (access(p.runner.c_str(), X_OK) || access(p.sdk.c_str(), R_OK)) {
        p.autorun_result("failed", "开机运行器或 SDK 不可用");
        return;
    }
    p.autorun_motion_seq = motion_seq_now(p.ctx);
    p.autorun_result("waiting", "等待底盘连接并确认静止");
    try {
        p.autorun_worker = std::thread([this, cfg] {
            auto& state = *impl_;
            const double until = now_ms() + cfg.startup_timeout_ms;
            try {
                while (!state.closing && !state.ctx.shutdown && state.autorun_pending()) {
                    if (motion_seq_now(state.ctx) != state.autorun_motion_seq) {
                        state.cancel_autorun("人工控制已取消本次开机运行"); return;
                    }
                    {
                        std::lock_guard<std::mutex> demo(state.ctx.script_mu);
                        if (state.ctx.script_running) {
                            state.cancel_autorun("Demo 已取消本次开机运行"); return;
                        }
                    }
                    if (now_ms() >= until) {
                        state.autorun_result("failed", "等待底盘就绪超时，本次开机不再重试"); return;
                    }
                    bool arm_busy;
                    { std::lock_guard<std::mutex> arm(state.ctx.arm_mu); arm_busy = state.ctx.arm_busy; }
                    if (!arm_busy && state.ctx.motor_pair->move_state() != 1 && state.stationary()) {
                        if (now_ms() >= until) continue;
                        int code = 0;
                        const Json result = run_internal(cfg.autorun_program_id, code, true);
                        if (code == 202) state.autorun_result("started", "本次开机已自动启动一次");
                        else if (state.autorun_pending()) state.autorun_result("failed", result.gets("error"));
                        return;
                    }
                    usleep(50000);
                }
                state.cancel_autorun("服务关闭，取消本次开机运行");
            } catch (const std::exception& e) {
                if (state.autorun_pending()) state.autorun_result("failed", e.what());
            }
        });
    } catch (const std::exception& e) {
        p.autorun_result("failed", e.what());
    }
}
void ProgramService::shutdown() {
    auto& p = *impl_;
    p.closing = true;
    p.abort = true;
    p.cancel_autorun("服务关闭，取消本次开机运行");
    // The scheduler may be entering run_internal(), which takes admin.
    // Join it before taking that mutex; never detach a thread holding this/ctx.
    if (p.autorun_worker.joinable()) p.autorun_worker.join();
    std::lock_guard<std::mutex> admin(p.admin);
    p.abort = true;
    if (p.worker.joinable()) p.worker.join();
}
}  // namespace capp
