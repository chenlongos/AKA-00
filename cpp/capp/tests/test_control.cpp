// Exercise the production timed-stop boundary used when program takes motion ownership.
#include "capp/context.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>

using namespace capp;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; \
} } while (0)

class TestMotor : public csrc::MotorPair {
public:
    void set_speed(int, int) override {}
    void get_speeds(int& left, int& right) override { left = right = 0; }
    void brake() override { sleep(); }
    void sleep() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++stops;
        changed.notify_all();
    }
    void close() override {}
    bool reinitialize() override { return true; }
    void get_encoder(int& left, int& right) override { left = right = 0; }
    bool wait_stops(int count, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [&] { return stops >= count; });
    }
private:
    std::mutex mutex;
    std::condition_variable changed;
    int stops = 0;
};

class TestGripper : public csrc::Gripper {
public:
    void open() override {}
    void close() override {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
        if (fail) throw std::runtime_error("test grab failure");
    }
    csrc::GripperStatus get_status() override { return csrc::GripperStatus::Unknown; }
    void update_angles(const csrc::Json&) override {}
    void preview_angle(const std::string&, int) override {}
    bool wait_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(1), [&] { return entered; });
    }
    void release(bool should_fail = false) {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        fail = should_fail;
        changed.notify_all();
    }
private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool fail = false;
};

// No device is opened; unused hardware/service functions are discarded by the linker.
namespace csrc {
Camera& Camera::get_instance() { static Camera camera; return camera; }
Camera::~Camera() = default;
ScreenDisplay::~ScreenDisplay() = default;
YoloDetector::~YoloDetector() = default;
}

int main() {
    AppContext ctx;
    auto* motor = new TestMotor;
    ctx.motor_pair.reset(motor);
    auto* gripper = new TestGripper;
    ctx.gripper.reset(gripper);

    run_motor(ctx, 20, 20, 0.5);
    const int64_t old_seq = motion_seq_now(ctx);
    run_motor(ctx, 37, 38, 0.08);
    // A stale program decision must leave the new human command's auto-stop intact.
    CHECK(!cancel_pending_stop_if_owned(ctx, old_seq));
    CHECK(motor->wait_stops(1, 1000));
    cancel_pending_stop(ctx);  // Also joins an already-finished timer without deadlocking.
    CHECK(ctx.collector.get_status().left_target == 0);
    CHECK(ctx.collector.get_status().right_target == 0);

    run_motor(ctx, 20, 20, 0.08);
    CHECK(cancel_pending_stop_if_owned(ctx, motion_seq_now(ctx)));
    CHECK(!motor->wait_stops(2, 150));

    // Even a zero-duration wait must check ownership at its final auto-stop boundary.
    const int64_t superseded = bump_motion_seq(ctx);
    bump_motion_seq(ctx);
    CHECK(wait_timed_done(ctx, superseded, 0) == 1);
    CHECK(!motor->wait_stops(2, 1));
    CHECK(ctx.collector.get_status().left_target == 20);
    CHECK(ctx.collector.get_status().right_target == 20);
    CHECK(wait_timed_done(ctx, motion_seq_now(ctx), 0) == 0);
    CHECK(motor->wait_stops(2, 1));
    CHECK(ctx.collector.get_status().left_target == 0);
    CHECK(ctx.collector.get_status().right_target == 0);
    ctx.shutdown = true;
    CHECK(wait_timed_done(ctx, motion_seq_now(ctx), 0) == 2);
    CHECK(!motor->wait_stops(3, 1));
    cancel_pending_stop(ctx);
    ctx.shutdown = false;

    CHECK(apply_arm_action(ctx, "grab") == ArmResult::Accepted);
    CHECK(gripper->wait_entered());
    CHECK(apply_arm_action(ctx, "grab") == ArmResult::Busy);
    CHECK(apply_arm_action(ctx, "release") == ArmResult::Busy);
    gripper->release();
    wait_arm_done(ctx);
    {
        std::lock_guard<std::mutex> lock(ctx.arm_mu);
        CHECK(!ctx.arm_busy && ctx.arm_error.empty());
    }
    auto* failing_gripper = new TestGripper;
    ctx.gripper.reset(failing_gripper);
    CHECK(apply_arm_action(ctx, "grab") == ArmResult::Accepted);
    CHECK(failing_gripper->wait_entered());
    failing_gripper->release(true);
    wait_arm_done(ctx);
    {
        std::lock_guard<std::mutex> lock(ctx.arm_mu);
        CHECK(!ctx.arm_busy && ctx.arm_error == "test grab failure");
    }

    std::printf("Control boundaries: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
