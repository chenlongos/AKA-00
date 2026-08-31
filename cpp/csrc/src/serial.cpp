// csrc/serial.cpp

#include "csrc/serial.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "csrc/log.hpp"

namespace csrc {

static speed_t baud_to_speed(int baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#if defined(B460800)
        case 460800: return B460800;
#endif
#if defined(B921600)
        case 921600: return B921600;
#endif
        default: return B115200;
    }
}

bool SerialPort::open(const std::string& port, int baudrate, double timeout_sec) {
    close();
    fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        err_ = "open " + port + ": " + std::strerror(errno);
        CAM_WARN("serial open %s failed: %s", port.c_str(), std::strerror(errno));
        return false;
    }

    struct termios tio;
    std::memset(&tio, 0, sizeof tio);
    if (tcgetattr(fd_, &tio) < 0) {
        err_ = "tcgetattr: " + std::string(std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    speed_t sp = baud_to_speed(baudrate);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);

    // raw 模式 8N1
    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;  // 非阻塞，超时由 select 控制

    if (tcsetattr(fd_, TCSANOW, &tio) < 0) {
        err_ = "tcsetattr: " + std::string(std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // 清掉历史残留（对应 pyserial reset_input_buffer）
    clear_input();
    clear_output();

    timeout_sec_ = timeout_sec;
    CAM_DEBUG("serial %s opened @%d baud", port.c_str(), baudrate);
    return true;
}

void SerialPort::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

/// 等待可读/可写，返回 0=超时, 1=就绪, -1=错误
static int wait_fd(int fd, short events, double timeout_sec) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    int timeout_ms = timeout_sec <= 0 ? 0 : (int)(timeout_sec * 1000.0 + 0.5);
    int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc < 0) return -1;
    if (rc == 0) return 0;
    return 1;
}

size_t SerialPort::read(uint8_t* buf, size_t n) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (fd_ < 0 || n == 0) return 0;
    int rc = wait_fd(fd_, POLLIN, timeout_sec_);
    if (rc <= 0) return 0;
    ssize_t got = ::read(fd_, buf, n);
    if (got <= 0) return 0;
    return (size_t)got;
}

bool SerialPort::read_exact(uint8_t* buf, size_t n) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    size_t got = 0;
    while (got < n) {
        if (fd_ < 0) return false;
        int rc = wait_fd(fd_, POLLIN, timeout_sec_);
        if (rc <= 0) return false;
        ssize_t r = ::read(fd_, buf + got, n - got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

bool SerialPort::write(const uint8_t* data, size_t n) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (fd_ < 0) return false;
    size_t sent = 0;
    while (sent < n) {
        int rc = wait_fd(fd_, POLLOUT, timeout_sec_);
        if (rc <= 0) return false;
        ssize_t w = ::write(fd_, data + sent, n - sent);
        if (w <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        sent += (size_t)w;
    }
    return true;
}

void SerialPort::clear_input() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (fd_ < 0) return;
    ::tcflush(fd_, TCIFLUSH);
}

void SerialPort::clear_output() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (fd_ < 0) return;
    ::tcflush(fd_, TCOFLUSH);
}

int SerialPort::in_waiting() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (fd_ < 0) return 0;
    int n = 0;
    if (::ioctl(fd_, FIONREAD, &n) < 0) return 0;
    return n;
}

}  // namespace csrc
