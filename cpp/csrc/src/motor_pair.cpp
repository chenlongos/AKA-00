// csrc/motor_pair.cpp

#include "csrc/motor_pair.hpp"

#include <chrono>

#include "csrc/log.hpp"

namespace csrc {

namespace {
/// 包装 TtPidChassis 为 MotorPair（TtPidChassis 本身即满足接口语义）
class TtPidMotorPair : public MotorPair {
public:
    explicit TtPidMotorPair(std::unique_ptr<TtPidChassis> chassis) : chassis_(std::move(chassis)) {}

    void set_speed(int left, int right) override { chassis_->set_speed(left, right); }
    void get_speeds(int& l, int& r) override { chassis_->get_speeds(l, r); }
    void brake() override { chassis_->brake(); }
    void sleep() override { chassis_->sleep(); }
    void close() override { chassis_->close(); }
    bool reinitialize() override { return chassis_->reinitialize(); }
    void get_encoder(int& c1, int& c2) override { chassis_->get_encoder(c1, c2); }
    void move_distance(uint8_t dir, uint8_t speed, int32_t target) override {
        chassis_->move_distance(dir, speed, target);
    }
    void send_cmd_noresp(uint8_t cmd, const uint8_t* payload, size_t len) override {
        chassis_->send_cmd_noresp(cmd, payload, len);
    }
    bool ping() override { return chassis_->ping(); }
    int move_state() const override { return chassis_->move_state(); }

private:
    std::unique_ptr<TtPidChassis> chassis_;
};

constexpr int64_t kPingIntervalMs = 1500;   // 已连接时的心跳间隔
constexpr int kPingFailsBeforeDrop = 2;     // 连续几次 ping 失败判定掉线
constexpr int64_t kBackoffBaseMs = 500;     // 重连退避起点
constexpr int64_t kBackoffCapMs = 30000;    // 重连退避上限
}  // namespace

// ─────────────────── AutoReconnectMotorPair ───────────────────

AutoReconnectMotorPair::AutoReconnectMotorPair(std::string port, int baudrate, int ppr,
                                               const std::string& backend)
    : backend_(backend),
      port_(std::move(port)),
      baudrate_(baudrate),
      ppr_(ppr) {
    mock_ = std::make_shared<MockMotorPair>();
    active_ = mock_;
    enabled_ = (backend_ == "tt_pid");
    if (enabled_) {
        // 后台立刻尝试首次连接（构造不阻塞、不抛异常 → 服务必然能起）
        worker_ = std::thread([this] { worker_loop(); });
        CAM_INFO("[motor] auto-reconnect enabled (port=%s baud=%d ppr=%d)",
                 port_.c_str(), baudrate_, ppr_);
    } else {
        CAM_INFO("[motor] backend=%s → mock (auto-reconnect off)", backend_.c_str());
    }
}

AutoReconnectMotorPair::~AutoReconnectMotorPair() { close(); }

std::shared_ptr<MotorPair> AutoReconnectMotorPair::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_;
}

// ── 转发方法：active() 永不为空（无真实驱动时为 mock），无需判空 ──

void AutoReconnectMotorPair::set_speed(int left, int right) {
    auto p = active();
    p->set_speed(left, right);
}

void AutoReconnectMotorPair::get_speeds(int& l, int& r) {
    auto p = active();
    p->get_speeds(l, r);
}

void AutoReconnectMotorPair::brake() {
    auto p = active();
    p->brake();
}

void AutoReconnectMotorPair::sleep() {
    auto p = active();
    p->sleep();
}

void AutoReconnectMotorPair::get_encoder(int& c1, int& c2) {
    auto p = active();
    p->get_encoder(c1, c2);
}

void AutoReconnectMotorPair::move_distance(uint8_t dir, uint8_t speed, int32_t target) {
    auto p = active();
    p->move_distance(dir, speed, target);
}

void AutoReconnectMotorPair::send_cmd_noresp(uint8_t cmd, const uint8_t* payload, size_t len) {
    auto p = active();
    p->send_cmd_noresp(cmd, payload, len);
}

MotorLinkStatus AutoReconnectMotorPair::link_status() const {
    std::lock_guard<std::mutex> lk(mu_);
    MotorLinkStatus st;
    st.backend = backend_;
    st.enabled = enabled_;
    st.connected = connected_;
    st.state = connected_ ? "connected" : (enabled_ ? "reconnecting" : "disabled");
    st.attempts = attempts_;
    st.error = error_;
    return st;
}

/// 断开当前真实驱动并换回 mock（连接状态清零）。
/// expected=nullptr 无条件断；非空时仅当 active_ 仍是 expected 才断（防误杀新链）。
bool AutoReconnectMotorPair::drop(const std::shared_ptr<MotorPair>& expected) {
    std::shared_ptr<MotorPair> old;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (expected && active_ != expected) return false;  // 已被他人重连/替换
        old = std::move(active_);
        active_ = mock_;
        connected_ = false;
    }
    if (old && old != mock_) {
        CAM_INFO("[motor] link dropped");
        old->close();
    }
    return true;
}

/// 打断退避/心跳并立刻重连（异步：断开 → worker 马上重试）
void AutoReconnectMotorPair::request_reconnect() {
    {
        std::lock_guard<std::mutex> attempt_lk(attempt_mu_);
        drop(nullptr);
        std::lock_guard<std::mutex> lk(mu_);
        wake_ = true;
    }
    cv_.notify_all();
}

bool AutoReconnectMotorPair::reinitialize() {
    // 语义：已连上 → 原地重发 INIT/CONFIG（清 PID/编码器，不掉线不打断运行）；
    // 未连上或原地重发失败 → 断开并完整重连一次（自愈）。纯 mock 直接成功。
    if (!enabled_) return true;
    std::lock_guard<std::mutex> attempt_lk(attempt_mu_);
    std::shared_ptr<MotorPair> cur;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (connected_) cur = active_;
    }
    if (cur && cur != mock_ && cur->reinitialize()) {
        return true;  // 链路健康：原地 INIT/CONFIG 成功
    }
    drop(cur);  // 仅当还是同一条链才断（防误杀并发重连的新链）
    bool ok = try_connect();
    {
        std::lock_guard<std::mutex> lk(mu_);
        wake_ = true;  // 让 worker 立即感知当前状态（连上→维护，失败→继续重试）
    }
    cv_.notify_all();
    return ok;
}

void AutoReconnectMotorPair::close() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        wake_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    drop(nullptr);
}

/// 同步尝试建连一次（attempt_mu_ 持有时调用）。
/// 成功后切 active_ 到真实驱动并清零错误计数。
bool AutoReconnectMotorPair::try_connect() {
    auto chassis = std::make_unique<TtPidChassis>(port_, baudrate_, ppr_, 20000);
    if (!chassis->ok()) {
        std::lock_guard<std::mutex> lk(mu_);
        attempts_++;
        error_ = chassis->error().empty() ? "ESP32 init failed" : chassis->error();
        CAM_WARN("[motor] connect attempt #%d failed: %s", attempts_, error_.c_str());
        return false;
    }
    auto real = std::make_shared<TtPidMotorPair>(std::move(chassis));
    std::shared_ptr<MotorPair> old;
    {
        std::lock_guard<std::mutex> lk(mu_);
        old = std::move(active_);
        active_ = real;
        connected_ = true;
        attempts_ = 0;
        error_.clear();
    }
    if (old && old != mock_) old->close();
    CAM_INFO("[motor] ✓ real chassis connected (%s)", port_.c_str());
    return true;
}

void AutoReconnectMotorPair::wait_cancelable(int64_t ms) {
    std::unique_lock<std::mutex> lk(mu_);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!stop_ && !wake_) {
        // 分段等待，保证 stop/wake 的响应延迟 ~≤250ms
        const auto now = std::chrono::steady_clock::now();
        if (now >= until) break;
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(until - now);
        if (remain.count() > 250) remain = std::chrono::milliseconds(250);
        cv_.wait_for(lk, remain);
    }
    if (wake_) wake_ = false;
}

void AutoReconnectMotorPair::worker_loop() {
    if (!enabled_) return;

    while (true) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stop_) return;
            wake_ = false;
        }

        // ── 未连接 → 尝试建连；已连接（如 reinitialize 刚连上）→ 直接维护 ──
        bool connected;
        {
            std::lock_guard<std::mutex> lk(mu_);
            connected = connected_;
        }
        if (!connected) {
            {
                std::lock_guard<std::mutex> attempt_lk(attempt_mu_);
                if (try_connect()) connected = true;
            }
            if (!connected) {
                int64_t backoff;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    backoff = kBackoffBaseMs << (attempts_ > 0 ? (attempts_ - 1) : 0);
                    if (backoff > kBackoffCapMs) backoff = kBackoffCapMs;
                }
                wait_cancelable(backoff);  // 可被 request_reconnect/close 打断
                continue;
            }
        }

        // ── 已连接：周期探活，连续失败判定掉线 → 回到连接循环 ──
        int fails = 0;
        while (true) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (stop_) return;
                if (wake_) {  // request_reconnect：立刻重连
                    wake_ = false;
                    break;
                }
            }
            auto p = active();
            bool alive = p && p->ping();
            if (!alive) {
                if (++fails >= kPingFailsBeforeDrop) {
                    CAM_WARN("[motor] heartbeat lost %d times → reconnecting", fails);
                    std::lock_guard<std::mutex> attempt_lk(attempt_mu_);
                    drop(p);  // 只断自己探测的这条链路
                    break;
                }
            } else {
                fails = 0;
            }
            wait_cancelable(kPingIntervalMs);
        }
    }
}

std::unique_ptr<MotorPair> create_motor_pair(const std::string& port,
                                             const std::string& backend,
                                             int baudrate, int ppr) {
    if (backend == "tt_pid") {
        return std::make_unique<AutoReconnectMotorPair>(port, baudrate, ppr, backend);
    }
    CAM_INFO("[motor] backend=%s → mock", backend.c_str());
    return std::make_unique<MockMotorPair>();
}

}  // namespace csrc
