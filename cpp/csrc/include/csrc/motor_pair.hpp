// csrc/motor_pair.hpp — 双轮底盘抽象接口 + 工厂（对应 src/base_control/interfaces.py）

#pragma once

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

    /// 闭环距离/转向（仅 tt_pid 支持，mock 忽略）
    virtual void move_distance(uint8_t dir, uint8_t speed, int32_t target) {
        (void)dir; (void)speed; (void)target;
    }
    /// 发原始帧（仅 tt_pid 支持）
    virtual void send_cmd_noresp(uint8_t cmd, const uint8_t* payload, size_t len) {
        (void)cmd; (void)payload; (void)len;
    }
    /// 探活：链路往返成功返回 true（mock 恒 true；自动重连线程使用）
    virtual bool ping() { return true; }
    /// 闭环距离/转向状态（tt_pid 真实底盘支持；mock/未知返回 -1）:
    ///   0=空闲 1=运行中 2=完成(到达目标) 3=中止(失联/重置)
    virtual int move_state() const { return -1; }
};

/// Mock 底盘（开发机，无真实硬件）：打印命令
class MockMotorPair : public MotorPair {
public:
    void set_speed(int left, int right) override {
        CAM_INFO("[MockMotorPair] set_speed(left=%d, right=%d)", left, right);
        last_left_ = left; last_right_ = right;
    }
    void get_speeds(int& l, int& r) override { l = last_left_; r = last_right_; }
    void brake() override { CAM_INFO("[MockMotorPair] brake()"); last_left_ = last_right_ = 0; }
    void sleep() override { CAM_INFO("[MockMotorPair] sleep()"); last_left_ = last_right_ = 0; }
    void close() override {}
    bool reinitialize() override { return true; }
    void get_encoder(int& c1, int& c2) override { c1 = c2 = 0; }

private:
    int last_left_ = 0;
    int last_right_ = 0;
};

/// 底盘连接状态（/api/motor/status 等对外暴露用）
struct MotorLinkStatus {
    std::string backend;    // 配置的 backend（如 "tt_pid"）
    bool enabled = false;   // backend != dev：需要真实硬件
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
///   - 已连接后周期 ping 探活，连续失败判定掉线，自动换回 mock 并重连；
///   - reinitialize() = 断开当前连接并立即重连一次（可被前端调用）。
/// backend 为 "dev"（或非 tt_pid）时退化为纯 mock，不启动后台线程。
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
    /// 断开当前真实驱动并换回 mock（连接状态清零）。
    /// expected=nullptr 无条件断开；非空时仅当 active_ 仍是 expected 才断开
    /// （防误杀并发重连出的新链路）。返回是否真的断开了。
    bool drop(const std::shared_ptr<MotorPair>& expected);
    std::shared_ptr<MotorPair> active() const;  // 拷贝当前驱动（线程安全）
    /// 分段等待可被 close()/request_reconnect() 打断
    void wait_cancelable(int64_t ms);

    const std::string backend_;
    const std::string port_;
    int baudrate_;
    int ppr_;

    // 驱动切换保护（命令线程与 worker 并发访问 active_）
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool wake_ = false;  // request_reconnect/close 置位，打断退避与心跳
    std::shared_ptr<MockMotorPair> mock_;
    std::shared_ptr<MotorPair> active_;

    // 连接尝试串行化（worker 与 reinitialize() 不能同时开串口）
    std::mutex attempt_mu_;

    // 后台线程（enabled 时启动）
    std::thread worker_;

    // 状态（mu_ 保护）
    bool enabled_ = false;
    bool connected_ = false;
    int attempts_ = 0;
    std::string error_;
};

/// 创建底盘。backend: "tt_pid"（ESP32 编码器，自动重连）或 "dev"（开发用 mock）。
std::unique_ptr<MotorPair> create_motor_pair(const std::string& port,
                                             const std::string& backend,
                                             int baudrate = 115200, int ppr = 4680);

}  // namespace csrc
