// csrc/motor_pair.hpp — 双轮底盘抽象接口 + 工厂（对应 src/base_control/interfaces.py）

#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "csrc/log.hpp"
#include "csrc/tt_pid.hpp"

namespace csrc {

class MotorPair {
public:
    virtual ~MotorPair() = default;

    virtual void set_speed(int left, int right) = 0;
    virtual void get_speeds(int& left_rpm, int& right_rpm) = 0;
    virtual void brake() = 0;
    virtual void sleep() = 0;
    virtual void close() = 0;
    virtual bool reinitialize() = 0;
    virtual void get_encoder(int& c1, int& c2) = 0;

    /// 闭环距离/转向（仅 tt_pid 支持；没连上时由代理丢弃）
    virtual void move_distance(uint8_t dir, uint8_t speed, int32_t target) {
        (void)dir; (void)speed; (void)target;
    }
    /// 发原始帧（仅 tt_pid 支持）
    virtual void send_cmd_noresp(uint8_t cmd, const uint8_t* payload, size_t len) {
        (void)cmd; (void)payload; (void)len;
    }
    /// 探活：链路往返成功返回 true（自动重连线程使用）
    virtual bool ping() { return true; }
    /// 闭环距离/转向状态（tt_pid 真实底盘支持；未知返回 -1）:
    ///   0=空闲 1=运行中 2=完成(到达目标) 3=中止(失联/重置)
    virtual int move_state() const { return -1; }
};

/// 底盘连接状态（/api/motor/status 等对外暴露用）
struct MotorLinkStatus {
    std::string backend;    // 配置的 backend（如 "tt_pid"）
    bool enabled = true;    // 底盘控制是否启用（现在恒 true：要么连上，要么报未连接）
    bool connected = false; // 当前是否已连上真实底盘
    std::string state = "disabled";  // connected / reconnecting / disabled
    int attempts = 0;       // 当前连续失败次数（成功后清零）
    std::string error;      // 最近一次失败原因（空 = 无）
};

/// 自动重连底盘代理。
///
/// 解决"启动时 UART 瞬时失败 → 服务起不来 / 永久降级 mock"的问题：
///   - 构造永不抛异常，服务必然能起；
///   - 后台线程按退避策略反复尝试连接真实底盘（tt_pid 握手），连上即切换；
///   - 已连接后周期 ping 探活，连续失败判定掉线，自动断开并重连；
///   - reinitialize() = 断开当前连接并立即重连一次（可被前端调用）。
///
/// **没有 mock 兜底**（2026-09-18 删除）：以前连不上/配置写了 dev 时会退化成
/// MockMotorPair，指令被它吞掉 —— 车不动，而界面和接口都不报错（现场就踩过：
/// config.toml 被写成了 0 字节 → backend 取默认 "dev" → 一路 mock 到没人知道）。
/// 现在 active_ 空就是空：指令丢弃、记 ERROR、/api/motor/status 报未连接，
/// 界面那条已有的"底盘未连接"提示就会亮。backend 只认 "tt_pid"。
class AutoReconnectMotorPair : public MotorPair {
public:
    AutoReconnectMotorPair(std::string port, int baudrate, int ppr,
                           const std::string& backend);
    ~AutoReconnectMotorPair() override;

    // ── MotorPair ──
    void set_speed(int left, int right) override;
    void get_speeds(int& left_rpm, int& right_rpm) override;
    void brake() override;
    void sleep() override;
    void close() override;
    bool reinitialize() override;
    void get_encoder(int& c1, int& c2) override;
    void move_distance(uint8_t dir, uint8_t speed, int32_t target) override;
    void send_cmd_noresp(uint8_t cmd, const uint8_t* payload, size_t len) override;

    /// 线程安全读取当前连接状态
    MotorLinkStatus link_status() const;

    /// 打断退避等待并立刻重试连接（异步；drop + 唤醒 worker）
    void request_reconnect();

private:
    void worker_loop();
    /// 同步尝试连接一次（必须在 attempt_mu_ 持有时调用）；成功则切换 active_。
    bool try_connect();
    /// 断开当前真实驱动（active_ 置空、连接状态清零）。
    /// expected=nullptr 无条件断开；非空时仅当 active_ 仍是 expected 才断开
    /// （防误杀并发重连出的新链路）。返回是否真的断开了。
    bool drop(const std::shared_ptr<MotorPair>& expected);
    std::shared_ptr<MotorPair> active() const;  // 拷贝当前驱动（线程安全）
    /// 分段等待可被 close()/request_reconnect() 打断
    void wait_cancelable(int64_t ms);
    /// 底盘没连上、驱动指令被丢弃时告警，节流 ≤1 条/秒
    void warn_if_no_chassis(const char* what);

    const std::string backend_;
    const std::string port_;
    int baudrate_;
    int ppr_;

    // 驱动切换保护（命令线程与 worker 并发访问 active_）
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool wake_ = false;  // request_reconnect/close 置位，打断退避与心跳
    std::shared_ptr<MotorPair> active_;   // 空 = 没有真底盘（指令丢弃并报错）

    // 连接尝试串行化（worker 与 reinitialize() 不能同时开串口）
    std::mutex attempt_mu_;

    // 后台线程（enabled 时启动）
    std::thread worker_;

    // 状态（mu_ 保护）
    bool enabled_ = false;
    bool connected_ = false;
    int attempts_ = 0;
    std::string error_;
    std::chrono::steady_clock::time_point no_chassis_warn_at_{};  // 指令丢弃告警的节流
};

/// 创建底盘。backend 只认 "tt_pid"（ESP32 编码器，自动重连）；其它值会被明确报错，
/// 不再退化成 mock（mock 已删除）。
std::unique_ptr<MotorPair> create_motor_pair(const std::string& port,
                                             const std::string& backend,
                                             int baudrate = 115200, int ppr = 4680);

}  // namespace csrc
