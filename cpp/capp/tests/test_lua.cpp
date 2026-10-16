#include "capp/context.hpp"
#include "capp/program_service.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

int main() {
    char directory[] = "/tmp/aka-upstream-lua-XXXXXX";
    const char* root = mkdtemp(directory);
    assert(root);
    std::filesystem::create_directory(std::string(root) + "/demo");
    capp::AppContext ctx;
    ctx.app_dir = root;
    auto run = [&](const std::string& source, const std::string& expected,
                   const std::string& message) {
        std::ofstream(std::string(root) + "/demo/validation.lua") << source;
        csrc::Json params;
        params["n"] = 10;
        assert(capp::script_run(ctx, "validation", params).getb("ok"));
        csrc::Json status;
        for (int i = 0; i < 2000; ++i) {
            status = capp::script_status(ctx);
            if (status.gets("state") != "running") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(status.gets("state") == expected);
        assert(status.gets("message").find(message) != std::string::npos);
        assert(pthread_join(ctx.script_tid, nullptr) == 0);
        ctx.script_tid_valid = false;
    };
    run("local sum=0; for i=1,100000 do sum=sum+i end; return tostring(sum)",
        "done", "5000050000");
    run("local p=params(); local n=0; for i=1,p.n do n=n+i end; return 'lua-ok:'..n",
        "done", "lua-ok:55");
    run("assert(os==nil and io==nil and package==nil and require==nil); return 'sandbox-ok'",
        "done", "sandbox-ok");
    run("this is not valid Lua", "failed", "error:");
    std::filesystem::create_directory(std::string(root) + "/blockly");
    std::filesystem::copy_file("../board/blockly/robot-sdk.js", std::string(root) + "/blockly/robot-sdk.js");
    setenv("AKA_BLOCKLY_RUNNER", std::filesystem::absolute("../blockly/bin/aka-blockly-runner-dev").c_str(), 1);
    unsetenv("AKA_PROGRAM_DIR"); unsetenv("AKA_PROGRAM_VOLATILE");
    ctx.program_service = std::make_shared<capp::ProgramService>(ctx);
    csrc::Json bundle;
    bundle["formatVersion"] = 1; bundle["sdkVersion"] = 1;
    bundle["program"] = "await robot.wait(2);";
    bundle["requiredCapabilities"] = csrc::Json(csrc::Json::Type::Array);
    bundle["settings"]["moveSpeed"] = 20; bundle["settings"]["turnSpeed"] = 20;
    bundle["settings"]["maxRuntimeMs"] = 3000;
    int code = 0;
    const auto saved = ctx.program_service->upload(bundle, code);
    assert(code == 200);
    std::ofstream(std::string(root) + "/demo/validation.lua") << "while not abort_requested() do end";
    assert(capp::script_run(ctx, "validation", csrc::Json()).getb("ok"));
    ctx.program_service->run(saved.gets("programId"), code);
    assert(code == 409);
    capp::join_script_worker(ctx);
    ctx.program_service->run(saved.gets("programId"), code);
    assert(code == 202);
    assert(!capp::script_run(ctx, "validation", csrc::Json()).getb("ok"));
    ctx.program_service->shutdown();
    assert(!ctx.program_running);
    assert(ctx.program_service->status().gets("state") == "stopped");
    ctx.shutdown = true;
    assert(!capp::script_run(ctx, "validation", csrc::Json()).getb("ok"));
    ctx.program_service.reset();
    std::filesystem::remove_all(root);
    std::puts("Lua: instruction hook, parameters, sandbox, errors and Blockly exclusion/shutdown: PASSED");
}
