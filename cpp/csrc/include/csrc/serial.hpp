// csrc/serial.hpp — POSIX 串口封装（termios 8N1 + select 超时读写）
//
// 与 pyserial 语义对齐：
//   - open(port, baudrate, timeout_sec)
//   - read(n) 最多读 n 字节，超时返回 0
//   - read_exact(n) 读满 n 字节（配合超时）
//   - write / flush / clear_input / clear_output / close
//   - in_waiting() 可读字节数
//
// 非 POSIX（Windows/macOS 之外的编译目标）不提供实现；macOS 上可用。

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace csrc {

/// POSIX 串口封装（termios 8N1 + poll 超时）。
/// 线程安全：每个操作（read/write/clear）内部持锁；命令序列级原子性
/// 由上层驱动（如 TtPidChassis）的 io 锁保证。
class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort() { close(); }

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    /// 打开串口。timeout_sec 为读写超时（秒，0 表示非阻塞）。
    bool open(const std::string& port, int baudrate, double timeout_sec = 0.1);
    void close();
    bool is_open() const { return fd_ >= 0; }

    /// 读最多 n 字节，返回实际读取字节数（超时/错误返回 0）。
    size_t read(uint8_t* buf, size_t n);
    /// 读满 n 字节，超时或不足返回 false。
    bool read_exact(uint8_t* buf, size_t n);
    bool write(const uint8_t* data, size_t n);
    bool write(const std::string& s) { return write((const uint8_t*)s.data(), s.size()); }
    void flush() {}  // termios 下 write 直通内核，无需 flush
    void clear_input();
    void clear_output();
    int in_waiting();

    const std::string& error() const { return err_; }

private:
    int fd_ = -1;
    double timeout_sec_ = 0.1;
    std::string err_;
    std::recursive_mutex mu_;   // 递归锁：open() 内部会调 clear_input/clear_output
};

}  // namespace csrc
