// Isolated JS worker. stdin/stdout are a bounded JSON RPC channel to aka-capp.
// No filesystem, shell, networking, or module loader is exposed to student JS.
#include "quickjs.h"
#include "csrc/json.hpp"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace {
volatile sig_atomic_t cancelled = 0;
void stop(int) { cancelled = 1; }
double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Runtime { double deadline; bool done = false; bool failed = false; };
int interrupt(JSRuntime*, void* opaque) {
    return cancelled || now_ms() >= static_cast<Runtime*>(opaque)->deadline;
}
bool read_file(const char* path, std::string& value, size_t limit) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    char buffer[4096];
    while (file.read(buffer, sizeof buffer) || file.gcount()) {
        value.append(buffer, file.gcount());
        if (value.size() > limit) return false;
    }
    return !file.bad();
}
void emit(const csrc::Json& json) {
    const std::string line = json.dump() + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}
void exception(JSContext* ctx) {
    JSValue error = JS_GetException(ctx);
    JSValue stack = JS_GetPropertyStr(ctx, error, "stack");
    const char* message = JS_ToCString(ctx, error);
    const char* trace = JS_IsUndefined(stack) ? nullptr : JS_ToCString(ctx, stack);
    csrc::Json event;
    event["type"] = "error";
    event["error"] = std::string(message ? message : "JavaScript error").substr(0, 2048);
    if (trace) event["stack"] = std::string(trace).substr(0, 2048);
    emit(event);
    if (message) JS_FreeCString(ctx, message);
    if (trace) JS_FreeCString(ctx, trace);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, error);
}
JSValue call(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (cancelled) return JS_ThrowInternalError(ctx, "program cancelled");
    if (argc != 2) return JS_ThrowTypeError(ctx, "invalid robot call");
    JSValue request = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, request, "type", JS_NewString(ctx, "call"));
    JS_SetPropertyStr(ctx, request, "method", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, request, "args", JS_DupValue(ctx, argv[1]));
    JSValue json = JS_JSONStringify(ctx, request, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, request);
    if (JS_IsException(json)) return json;
    size_t length = 0;
    const char* text = JS_ToCStringLen(ctx, &length, json);
    if (!text) { JS_FreeValue(ctx, json); return JS_EXCEPTION; }
    if (length > 16384) {
        JS_FreeCString(ctx, text); JS_FreeValue(ctx, json);
        return JS_ThrowRangeError(ctx, "robot call exceeds 16 KiB");
    }
    std::fwrite(text, 1, length, stdout); std::fputc('\n', stdout); std::fflush(stdout);
    JS_FreeCString(ctx, text); JS_FreeValue(ctx, json);
    std::string response;
    auto* runtime = static_cast<Runtime*>(JS_GetContextOpaque(ctx));
    while (!cancelled && now_ms() < runtime->deadline) {
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        int ready = poll(&fd, 1, 20);
        if (ready < 0) continue;
        if (!ready) continue;
        char byte;
        if (read(STDIN_FILENO, &byte, 1) != 1) return JS_ThrowInternalError(ctx, "program service disconnected");
        if (byte != '\n') {
            response += byte;
            if (response.size() > 16384) return JS_ThrowRangeError(ctx, "invalid robot response");
            continue;
        }
        JSValue result = JS_ParseJSON(ctx, response.data(), response.size(), "robot-response");
        if (JS_IsException(result)) return result;
        JSValue ok = JS_GetPropertyStr(ctx, result, "ok");
        const bool success = JS_ToBool(ctx, ok) > 0;
        JS_FreeValue(ctx, ok);
        if (!success) {
            JSValue error = JS_GetPropertyStr(ctx, result, "error");
            const char* message = JS_ToCString(ctx, error);
            JSValue failure = JS_ThrowInternalError(ctx, "%s", message ? message : "robot action failed");
            if (message) JS_FreeCString(ctx, message);
            JS_FreeValue(ctx, error); JS_FreeValue(ctx, result);
            return failure;
        }
        JSValue value = JS_GetPropertyStr(ctx, result, "value");
        JS_FreeValue(ctx, result);
        return value;
    }
    return JS_ThrowInternalError(ctx, cancelled ? "program cancelled" : "program time limit exceeded");
}
JSValue done(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* runtime = static_cast<Runtime*>(JS_GetContextOpaque(ctx));
    runtime->done = true;
    runtime->failed = argc && JS_ToBool(ctx, argv[0]) > 0;
    csrc::Json event;
    event["type"] = runtime->failed ? "error" : "done";
    if (runtime->failed && argc > 1) {
        const char* error = JS_ToCString(ctx, argv[1]);
        event["error"] = std::string(error ? error : "JavaScript error").substr(0, 2048);
        if (error) JS_FreeCString(ctx, error);
    }
    emit(event);
    return JS_UNDEFINED;
}
}  // namespace

int main(int argc, char** argv) {
    const bool check = argc == 4 && std::string(argv[1]) == "--check";
    if ((!check && argc != 3) || (argc == 4 && !check)) return 64;
    signal(SIGTERM, stop); signal(SIGINT, stop); signal(SIGPIPE, SIG_IGN);
    std::string data, sdk;
    if (!read_file(argv[check ? 2 : 1], data, 512 * 1024) ||
        !read_file(argv[check ? 3 : 2], sdk, 64 * 1024)) return 66;
    csrc::Json bundle;
    if (!csrc::Json::parse(data, bundle) || !bundle.get("program")) return 65;
    const csrc::Json* settings = bundle.get("settings");
    const double maximum = settings ? settings->getd("maxRuntimeMs", 600000) : 600000;
    Runtime runtime{now_ms() + (check ? 3000 : maximum)};
    JSRuntime* rt = JS_NewRuntime();
    if (!rt) return 70;
    JS_SetMemoryLimit(rt, 8 * 1024 * 1024);
    JS_SetMaxStackSize(rt, 512 * 1024);
    JS_SetInterruptHandler(rt, interrupt, &runtime);
    JSContext* ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return 70; }
    JS_SetContextOpaque(ctx, &runtime);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "_robotCall", JS_NewCFunction(ctx, call, "_robotCall", 2));
    JS_SetPropertyStr(ctx, global, "_programDone", JS_NewCFunction(ctx, done, "_programDone", 2));
    JS_FreeValue(ctx, global);
    const std::string source = "\"use strict\";\n" + sdk +
        "\n(async () => {\n" + bundle.gets("program") +
        "\n})().then(() => _programDone(false), e => _programDone(true, String(e) + '\\n' + (e.stack || '')));\n";
    JSValue result = JS_Eval(ctx, source.data(), source.size(), "blockly-program.js",
                            JS_EVAL_TYPE_GLOBAL | (check ? JS_EVAL_FLAG_COMPILE_ONLY : 0));
    if (JS_IsException(result)) { exception(ctx); runtime.failed = true; }
    JS_FreeValue(ctx, result);
    if (!check && !runtime.failed) {
        while (!runtime.done && !cancelled && now_ms() < runtime.deadline) {
            JSContext* job_ctx = nullptr;
            int executed = JS_ExecutePendingJob(rt, &job_ctx);
            if (executed < 0) { exception(job_ctx); runtime.failed = true; break; }
            if (executed == 0) { runtime.failed = true; break; }
        }
        if (!runtime.done) runtime.failed = true;
    }
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    return cancelled ? 2 : (runtime.failed ? 1 : 0);
}
