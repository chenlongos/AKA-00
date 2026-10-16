// Real service, HTTP routes, isolated JS process, and production motion arbitration.
// Only hardware feedback and device handles are replaced; no physical device is opened.
#include "capp/context.hpp"
#include "capp/program_service.hpp"
#include "capp/http_server.hpp"
#include "routes_internal.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>

using namespace capp;
using csrc::Json;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; \
} } while (0)
namespace {
std::atomic<bool> connected{true};
class TestMotor : public csrc::MotorPair {
public:
    std::atomic<int> moves{0}, stops{0}, left{0}, right{0};
    std::atomic<int> distance_state{0};
    std::atomic<bool> fail{false}, responsive{true};
    void set_speed(int l, int r) override {
        if (fail) throw std::runtime_error("test motor failure");
        left = l; right = r; if (l || r) ++moves;
    }
    void get_speeds(int& l, int& r) override { l = left; r = right; }
    void brake() override { left = right = 0; ++stops; }
    void sleep() override { brake(); }
    void close() override {}
    bool reinitialize() override { return true; }
    bool ping() override { return responsive; }
    void get_encoder(int& l, int& r) override { l = r = 0; }
    int move_state() const override { return distance_state; }
    // Intentionally don't update the status: old 'done' must not complete a new move.
    void move_distance(uint8_t, uint8_t, int32_t) override { ++moves; }
};
class TestGripper : public csrc::Gripper {
public:
    std::atomic<int> grabs{0};
    std::atomic<bool> reachable{true};
    bool ping() override { return reachable; }
    void open() override {}
    void close() override { ++grabs; }
    csrc::GripperStatus get_status() override { return csrc::GripperStatus::Unknown; }
    void update_angles(const Json&) override {}
    void preview_angle(const std::string&, int) override {}
};
bool until(const std::function<bool()>& predicate, int limit = 3000) {
    for (int i = 0; i < limit / 5; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}
struct Fixture {
    AppContext ctx;
    TestMotor* motor = new TestMotor;
    TestGripper* gripper = new TestGripper;
    fs::path root;
    Router router;
    explicit Fixture(const fs::path& sdk) {
        char path[] = "/tmp/aka-program-test-XXXXXX";
        root = mkdtemp(path);
        fs::create_directories(root / "blockly");
        fs::copy_file(sdk, root / "blockly/robot-sdk.js");
        ctx.app_dir = root.string();
        ctx.motor_pair.reset(motor); ctx.gripper.reset(gripper);
        connected = true;
        routes::register_program_routes(router, ctx);
    }
    ~Fixture() { ctx.program_service->shutdown(); wait_arm_done(ctx); cancel_pending_stop(ctx); fs::remove_all(root); }
    Json request(const std::string& method, const std::string& path, const Json& json, int expected) {
        HttpRequest req; req.method = method; req.path = path; req.body = json.dump();
        HttpResponse response; ClientConn connection;
        CHECK(router.dispatch(method, path, req, response, connection, ctx));
        if (response.status != expected) std::fprintf(stderr, "HTTP %d expected %d: %s\n", response.status, expected, response.body.c_str());
        CHECK(response.status == expected);
        Json out; CHECK(Json::parse(response.body, out)); return out;
    }
    Json bundle(const std::string& js, int runtime = 10000) {
        Json b; b["formatVersion"] = 1; b["sdkVersion"] = 1; b["name"] = "测试"; b["program"] = js;
        b["requiredCapabilities"] = Json(Json::Type::Array);
        b["settings"]["moveSpeed"] = 20; b["settings"]["turnSpeed"] = 20; b["settings"]["maxRuntimeMs"] = runtime;
        return b;
    }
    Json upload(const Json& b, int expected = 200) { return request("POST", "/api/program/upload", b, expected); }
    Json start(int expected = 202) {
        Json b; b["programId"] = ctx.program_service->status().gets("programId");
        return request("POST", "/api/program/run", b, expected);
    }
    Json stop(int expected = 202) {
        Json b; b["runId"] = ctx.program_service->status().gets("runId");
        return request("POST", "/api/program/stop", b, expected);
    }
    Json finish(const std::string& state) {
        CHECK(until([&] { return !ctx.program_running; }));
        Json result = ctx.program_service->status();
        if (result.gets("state") != state) std::fprintf(stderr, "State expected %s: %s\n", state.c_str(), result.dump().c_str());
        CHECK(result.gets("state") == state); return result;
    }
    void restart_service() {
        ctx.program_service->shutdown();
        router = Router();
        ctx.program_service.reset();
        routes::register_program_routes(router, ctx);
    }
    void configure_autorun(int timeout_ms = 2000) {
        ctx.config.blockly.autorun_enabled = true;
        ctx.config.blockly.autorun_program_id = ctx.program_service->status().gets("programId");
        ctx.config.blockly.startup_timeout_ms = timeout_ms;
        restart_service();
    }
    void schedule() { ctx.program_service->start_autorun((root / "boot-claim").string()); }
    std::string autorun_state() { return ctx.program_service->status().get("autorun")->gets("state"); }
};
void test(const fs::path& sdk) {
    Fixture f(sdk);
    CHECK(f.request("GET", "/api/program/capabilities", Json(), 200).getb("runnerAvailable"));
    auto saved = f.upload(f.bundle("let n = 0; async function add(v) { return v + 1; } for(let i=0;i<3;i++) n = await add(n); await robot.print(n);"));
    const std::string id = saved.gets("programId");
    CHECK(id.size() == 64);
    f.upload(f.bundle("let = ;"), 400);
    CHECK(f.ctx.program_service->status().gets("programId") == id);
    Json invalid = f.bundle("await robot.print(1);"); invalid["sdkVersion"] = 2; f.upload(invalid, 400);
    invalid["sdkVersion"] = 1.5; f.upload(invalid, 400);
    invalid = f.bundle("await robot.print(1);"); invalid["requiredCapabilities"].push_back("followLine"); f.upload(invalid, 400);
    invalid = f.bundle("await robot.print(1);"); invalid["settings"]["moveSpeed"] = "20"; f.upload(invalid, 400);
    Json wrong; wrong["programId"] = "old-id"; f.request("POST", "/api/program/run", wrong, 409);
    f.ctx.script_running = true; f.start(409); f.ctx.script_running = false;
    f.start(); f.finish("completed");
    auto logs = f.ctx.program_service->logs("", 0);
    bool printed = false; for (const auto& entry : logs.get("entries")->array()) if (entry.gets("message") == "3") printed = true;
    CHECK(printed);

    f.upload(f.bundle("await robot.wait(2); await robot.forwardSeconds(0.1);"));
    f.start(); f.upload(f.bundle("await robot.print('bad');"), 409); f.start(409);
    wrong["runId"] = "stale"; f.request("POST", "/api/program/stop", wrong, 409);
    f.stop(); f.finish("stopped"); CHECK(f.motor->moves == 0);

    f.upload(f.bundle("await robot.forwardSeconds(2); await robot.grab();")); f.start();
    CHECK(until([&] { return f.motor->moves > 0; }));
    f.stop(); f.finish("stopped"); CHECK(f.motor->left == 0 && f.gripper->grabs == 0);

    f.upload(f.bundle("await robot.forwardSeconds(2); await robot.grab();"));
    const int moves = f.motor->moves;
    f.start(); CHECK(until([&] { return f.motor->moves > moves; }));
    run_motor(f.ctx, 37, 38, 0);
    const int stops = f.motor->stops;
    f.finish("failed"); CHECK(f.motor->left == 37 && f.motor->right == 38 && f.motor->stops == stops);
    CHECK(f.gripper->grabs == 0); run_motor(f.ctx, 0, 0, 0);

    f.upload(f.bundle("await robot.forwardSeconds(0.1);")); f.motor->fail = true;
    f.start(); CHECK(f.finish("failed").gets("error").find("test motor failure") != std::string::npos); f.motor->fail = false;
    connected = false; f.start(); f.finish("failed"); connected = true;
    f.upload(f.bundle("await robot.grab(); await robot.forwardSeconds(0.1);"));
    f.gripper->reachable = false; const int before_arm = f.motor->moves;
    f.start(); CHECK(f.finish("failed").gets("error").find("UART2") != std::string::npos);
    CHECK(f.gripper->grabs == 0 && f.motor->moves == before_arm); f.gripper->reachable = true;

    f.motor->distance_state = 2;
    f.upload(f.bundle("await robot.forward(3); await robot.print('wrong stale done');", 200));
    f.start(); f.finish("failed");
    logs = f.ctx.program_service->logs("", 0);
    for (const auto& entry : logs.get("entries")->array())
        CHECK(entry.gets("message") != "wrong stale done");

    f.upload(f.bundle("while(true) {}", 150)); f.start(); f.finish("failed");
    f.upload(f.bundle("const list=[]; while(true) list.push('z'.repeat(100000));", 2000)); f.start(); f.finish("failed");
    f.upload(f.bundle("await robot.print([typeof window,typeof document,typeof fetch,typeof require,typeof std,typeof os].join(','));"));
    f.start(); f.finish("completed");
    logs = f.ctx.program_service->logs("", 0);
    printed = false; for (const auto& entry : logs.get("entries")->array())
        if (entry.gets("message") == "undefined,undefined,undefined,undefined,undefined,undefined") printed = true;
    CHECK(printed);
    f.upload(f.bundle("for(let i=0;i<300;i++) await robot.print(i);")); f.start(); f.finish("completed");
    logs = f.ctx.program_service->logs("", 0); CHECK(logs.get("entries")->size() == 256 && logs.getb("truncated"));
    CHECK(f.ctx.program_service->logs("stale-run", 0).get("entries")->size() == 0);

    f.upload(f.bundle("await robot.forwardSeconds(0.1);")); f.start(); f.finish("completed");
    const int64_t completed_seq = motion_seq_now(f.ctx);
    const int completed_stops = f.motor->stops;
    f.stop(200);
    CHECK(motion_seq_now(f.ctx) == completed_seq && f.motor->stops == completed_stops);

    // Persisted source survives recreation; programs never auto-start after a restart.
    f.upload(f.bundle("await robot.print('saved');"));
    const std::string saved_id = f.ctx.program_service->status().gets("programId");
    { ProgramService restored(f.ctx); CHECK(restored.status().gets("state") == "ready"); CHECK(restored.status().gets("programId") == saved_id); }
    Json last; last["state"] = "running"; last["programId"] = saved_id;
    std::ofstream(f.root / "programs/last-run.json") << last.dump();
    { ProgramService restored(f.ctx); CHECK(restored.status().gets("state") == "interrupted"); CHECK(!f.ctx.program_running); }
    std::ofstream(f.root / "programs/current.json") << "{\"formatVersion\":1}";
    { ProgramService restored(f.ctx); CHECK(restored.status().gets("state") == "failed"); int code; restored.run("missing", code); CHECK(code == 409); }
}

void test_autorun(const fs::path& sdk) {
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.schedule(); CHECK(f.autorun_state() == "disabled");
        CHECK(!fs::exists(f.root / "boot-claim") && f.motor->moves == 0);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1); await robot.print('auto done');"));
        f.configure_autorun(); connected = false; f.schedule(); f.schedule();
        CHECK(f.autorun_state() == "waiting"); CHECK(f.motor->moves == 0);
        connected = true;
        CHECK(until([&] { return f.autorun_state() == "started"; }));
        f.finish("completed"); CHECK(f.motor->moves == 1);
        CHECK(f.ctx.program_service->status().getb("stopConfirmed"));
        // Supervisor restart preserves the boot claim; a new boot clears tmpfs.
        f.restart_service(); f.schedule();
        CHECK(f.autorun_state() == "skipped" && !f.ctx.program_running);
        CHECK(f.ctx.program_service->status().gets("runId").empty());
        fs::remove(f.root / "boot-claim"); f.restart_service(); f.schedule();
        CHECK(until([&] { return f.autorun_state() == "started"; }));
        f.finish("completed"); CHECK(f.motor->moves == 2);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun(100); connected = false; f.schedule();
        CHECK(until([&] { return f.autorun_state() == "failed"; }));
        connected = true; f.schedule();
        CHECK(f.autorun_state() == "failed" && f.motor->moves == 0);
        f.restart_service(); f.schedule(); CHECK(f.autorun_state() == "skipped");
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun(180); f.motor->left = 5; f.schedule();
        CHECK(until([&] { return f.autorun_state() == "failed"; }));
        CHECK(f.motor->moves == 0 && f.motor->stops == 0 && f.motor->left == 5);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun(180); f.motor->distance_state = 1; f.schedule();
        CHECK(until([&] { return f.autorun_state() == "failed"; }));
        CHECK(f.motor->moves == 0 && f.motor->stops == 0);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun(); f.motor->responsive = false; f.schedule();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(f.autorun_state() == "waiting" && f.motor->moves == 0);
        f.motor->responsive = true;
        CHECK(until([&] { return f.autorun_state() == "started"; })); f.finish("completed");
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun();
        f.ctx.config.blockly.autorun_program_id = std::string(64, '0');
        f.restart_service(); f.schedule();
        CHECK(f.autorun_state() == "skipped" && f.motor->moves == 0);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);")); f.configure_autorun(0); f.schedule();
        CHECK(f.autorun_state() == "failed" && f.motor->moves == 0);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);")); f.configure_autorun();
        fs::remove(f.root / "blockly/robot-sdk.js"); f.schedule();
        CHECK(f.autorun_state() == "failed" && f.motor->moves == 0);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun(); connected = false; f.schedule();
        Json stale; stale["runId"] = "stale";
        f.request("POST", "/api/program/stop", stale, 409);
        CHECK(f.autorun_state() == "waiting");
        f.request("POST", "/api/program/stop", Json(Json::Type::Object), 200);
        connected = true;
        CHECK(f.autorun_state() == "cancelled" && f.motor->moves == 0);
        f.restart_service(); f.schedule(); CHECK(f.autorun_state() == "skipped");
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun(); connected = false; f.schedule();
        run_motor(f.ctx, 25, 25, 0);
        CHECK(until([&] { return f.autorun_state() == "cancelled"; }));
        connected = true;
        CHECK(f.motor->moves == 1 && f.motor->stops == 0 && f.motor->left == 25);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun(); connected = false; f.schedule();
        execute_action(f.ctx, "stop", 0, 0);
        CHECK(until([&] { return f.autorun_state() == "cancelled"; }));
        CHECK(f.motor->moves == 0);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun(); connected = false; f.schedule();
        f.upload(f.bundle("await robot.print('new program');")); connected = true;
        CHECK(f.autorun_state() == "cancelled" && f.motor->moves == 0 && !f.ctx.program_running);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.wait(0.2); await robot.print('manual');"));
        f.configure_autorun(); connected = false; f.schedule();
        f.start(); connected = true; f.finish("completed");
        CHECK(f.autorun_state() == "cancelled");
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun();
        { std::lock_guard<std::mutex> demo(f.ctx.script_mu); f.ctx.script_running = true; }
        f.schedule();
        CHECK(until([&] { return f.autorun_state() == "cancelled"; }));
        { std::lock_guard<std::mutex> demo(f.ctx.script_mu); f.ctx.script_running = false; }
        CHECK(f.motor->moves == 0);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.wait(1); await robot.forwardSeconds(0.1);"));
        f.configure_autorun(); f.schedule();
        CHECK(until([&] { return f.autorun_state() == "started"; }));
        f.stop(); f.finish("stopped"); CHECK(f.motor->moves == 0);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);")); f.configure_autorun();
        const auto missing_parent = f.root / "not-present" / "claim";
        f.ctx.program_service->start_autorun(missing_parent.string());
        CHECK(f.autorun_state() == "failed" && f.motor->moves == 0);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);")); f.configure_autorun();
        Json last; last["state"] = "running"; last["programId"] = f.ctx.config.blockly.autorun_program_id;
        std::ofstream(f.root / "programs/last-run.json") << last.dump();
        f.restart_service(); f.schedule();
        CHECK(f.autorun_state() == "skipped" && !f.ctx.program_running && f.motor->moves == 0);
    }
    {
        Fixture f(sdk);
        f.upload(f.bundle("await robot.forwardSeconds(0.1);"));
        f.configure_autorun(); connected = false; f.schedule();
        f.ctx.program_service->shutdown();
        CHECK(f.autorun_state() == "cancelled" && f.motor->moves == 0);
    }
    std::puts("Blockly autorun: readiness, pinning, once per boot, cancellation and shutdown checked");
}

void test_deploy(const fs::path& sdk) {
    {
        Fixture f(sdk); f.schedule(); // Select this fixture's private boot marker.
        CHECK(f.request("GET", "/api/program/capabilities", Json(), 200).getb("deployAvailable"));
        auto deployed = f.request("POST", "/api/program/deploy",
            f.bundle("await robot.wait(0.1); await robot.print('deployed');"), 202);
        const auto id = deployed.gets("programId");
        CHECK(deployed.getb("deployed") && id.size() == 64 && !deployed.gets("runId").empty());
        CHECK(deployed.get("autorun")->getb("enabled"));
        CHECK(deployed.get("autorun")->gets("programId") == id);
        CHECK(fs::is_directory(f.root / "boot-claim"));
        f.request("POST", "/api/program/deploy", f.bundle("await robot.print('other');"), 409);
        f.finish("completed");
        f.restart_service(); f.schedule();
        CHECK(f.autorun_state() == "skipped" && !f.ctx.program_running);
        CHECK(f.ctx.program_service->status().gets("programId") == id);
        // A new boot restores both code and its selection, even with config disabled.
        fs::remove(f.root / "boot-claim"); f.restart_service(); f.schedule();
        CHECK(until([&] { return f.autorun_state() == "started"; }));
        f.finish("completed");
        // Physical power loss during a deployed run must restart it on the NEXT
        // boot. Restarting only the service must continue to skip it.
        Json last; last["programId"] = id; last["state"] = "running";
        std::ofstream(f.root / "programs/last-run.json") << last.dump();
        f.restart_service(); f.schedule();
        CHECK(f.autorun_state() == "skipped" && !f.ctx.program_running);
        fs::remove(f.root / "boot-claim"); f.restart_service(); f.schedule();
        CHECK(until([&] { return f.autorun_state() == "started"; }));
        f.finish("completed");
        // Failed syntax checks must preserve the previous boot program.
        f.request("POST", "/api/program/deploy", f.bundle("let = ;"), 400);
        CHECK(f.ctx.program_service->status().gets("programId") == id);
        f.restart_service();
        CHECK(f.ctx.program_service->status().get("autorun")->gets("programId") == id);
        // A plain upload does not inherit deployment, even if the input forges it.
        Json plain = f.bundle("await robot.print('download only');"); plain["autorunOnBoot"] = true;
        f.upload(plain);
        fs::remove(f.root / "boot-claim"); f.restart_service(); f.schedule();
        CHECK(f.autorun_state() == "disabled" && !f.ctx.program_running);
    }
    {
        Fixture f(sdk); f.schedule(); f.upload(f.bundle("await robot.print('original');"));
        const auto id = f.ctx.program_service->status().gets("programId");
        connected = false;
        f.request("POST", "/api/program/deploy", f.bundle("await robot.print('new');"), 409);
        connected = true; f.motor->left = 5;
        f.request("POST", "/api/program/deploy", f.bundle("await robot.print('new');"), 409);
        f.motor->left = 0;
        { std::lock_guard<std::mutex> lock(f.ctx.arm_mu); f.ctx.arm_busy = true; }
        f.request("POST", "/api/program/deploy", f.bundle("await robot.print('new');"), 409);
        { std::lock_guard<std::mutex> lock(f.ctx.arm_mu); f.ctx.arm_busy = false; }
        { std::lock_guard<std::mutex> lock(f.ctx.script_mu); f.ctx.script_running = true; }
        f.request("POST", "/api/program/deploy", f.bundle("await robot.print('new');"), 409);
        { std::lock_guard<std::mutex> lock(f.ctx.script_mu); f.ctx.script_running = false; }
        f.ctx.config.blockly.startup_timeout_ms = 0;
        f.request("POST", "/api/program/deploy", f.bundle("await robot.print('new');"), 400);
        CHECK(f.ctx.program_service->status().gets("programId") == id);
        CHECK(f.motor->moves == 0);
    }
    {
        Fixture f(sdk);
        setenv("AKA_PROGRAM_VOLATILE", "1", 1); f.restart_service(); f.schedule();
        CHECK(!f.request("GET", "/api/program/capabilities", Json(), 200).getb("deployAvailable"));
        f.request("POST", "/api/program/deploy", f.bundle("await robot.print('volatile');"), 409);
        CHECK(!fs::exists(f.root / "programs/current.json"));
        unsetenv("AKA_PROGRAM_VOLATILE");
    }
    std::puts("Blockly deploy: durable publication, immediate run, boot recovery and rejection checked");
}

void test_autorun_config() {
    char path[] = "/tmp/aka-config-test-XXXXXX";
    const fs::path root = mkdtemp(path);
    const char* existing = getenv("AKA_HOME");
    const std::string saved = existing ? existing : "";
    const bool had_existing = existing != nullptr;
    setenv("AKA_HOME", root.c_str(), 1);
    std::ofstream(root / "config.toml") << "[blockly]\nautorun_enabled = true\nautorun_program_id = \"" <<
        std::string(64, 'a') << "\"\nstartup_timeout_ms = 12345\n";
    auto config = csrc::Config::load();
    CHECK(config.blockly.autorun_enabled && config.blockly.startup_timeout_ms == 12345);
    CHECK(config.blockly.autorun_program_id == std::string(64, 'a'));
    for (const auto& value : {"oops", "100ms", "0", "-1", "600001", "99999999999999999999"}) {
        std::ofstream(root / "config.toml") << "[blockly]\nstartup_timeout_ms = " << value << "\n";
        config = csrc::Config::load();
        CHECK(config.blockly.startup_timeout_ms == 0 && !config.blockly.autorun_enabled);
    }
    std::ofstream(root / "config.toml") << "[blockly]\nautorun_enabled = typo\n";
    CHECK(!csrc::Config::load().blockly.autorun_enabled);
    if (had_existing) setenv("AKA_HOME", saved.c_str(), 1); else unsetenv("AKA_HOME");
    fs::remove_all(root);
}
} // namespace
namespace capp {
Json motor_status_json(AppContext&) { Json j; j["connected"] = connected.load(); return j; }
}
namespace csrc {
Camera& Camera::get_instance() { static Camera camera; return camera; }
Camera::~Camera() = default;
ScreenDisplay::~ScreenDisplay() = default;
YoloDetector::~YoloDetector() = default;
}
int main(int argc, char** argv) {
    if (argc != 3) return 64;
    signal(SIGPIPE, SIG_IGN);
    setenv("AKA_BLOCKLY_RUNNER", fs::absolute(argv[1]).c_str(), 1);
    unsetenv("AKA_PROGRAM_DIR"); unsetenv("AKA_PROGRAM_VOLATILE");
    test(fs::absolute(argv[2]));
    test_autorun(fs::absolute(argv[2]));
    test_deploy(fs::absolute(argv[2]));
    test_autorun_config();
    std::printf("Blockly service + isolated process: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
