// HTTP 连接：ClientConn（读写/关闭）+ TLS BIO 回调 + poll_for
//
// 由 capp/src/http_server.cpp 拆出来（声明都还在 capp/http_server.hpp，类没变）。
// 服务端与 TLS 之外的这一层不关心业务；HttpServer 只通过 ClientConn 收发。

#include "capp/http_server.hpp"

#include "http_internal.hpp"   // 本文件定义它们（声明在这，好让编译器核对）

#include "csrc/log.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <poll.h>
#include <sys/socket.h>   // send/recv（MSG_NOSIGNAL 防 SIGPIPE）
#include <unistd.h>

namespace capp {


// ═══════════════════════ TLS helpers ═══════════════════════
//
// 自定义 BIO 把 mbedtls_ssl_* 的 I/O 重定向到 ClientConn 的 fd，并把 EAGAIN
// 翻译成 MBEDTLS_ERR_SSL_WANT_READ/WANT_WRITE，让上层 write_all / read_some
// 里的 poll 循环处理背压。SIGPIPE 已在 main.cpp 用 SIG_IGN 屏蔽，可直接 send()。

int tsl_bio_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    ssize_t n = ::send(fd, buf, len, 0);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

int tsl_bio_recv(void* ctx, unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    ssize_t n = ::recv(fd, buf, len, 0);
    if (n > 0) return (int)n;
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

// 把 mbedTLS 错误码转成可读字符串（调试用）
std::string tls_strerror(int code) {
    char buf[256];
    mbedtls_strerror(code, buf, sizeof buf);
    return std::string(buf);
}

// ═══════════════════════ ClientConn ═══════════════════════

// WANT_READ/WANT_WRITE 时的轮询辅助：成功返回 true，超时或硬错误返回 false。
bool poll_for(int fd, short events, int timeout_ms) {
    struct pollfd pfd = {fd, events, 0};
    int rc = ::poll(&pfd, 1, timeout_ms);
    return rc > 0;
}

bool ClientConn::write_all(const void* data, size_t len) {
    if (ssl) {
        // TLS：mbedtls_ssl_write 把明文缓冲成 TLS record 后调用 BIO 写出。
        // 短写/WANT_* 都必须重试，否则长连接（MJPEG）会在这里断。
        const unsigned char* p = (const unsigned char*)data;
        size_t sent = 0;
        while (sent < len) {
            int n = mbedtls_ssl_write(ssl, p + sent, len - sent);
            if (n > 0) { sent += (size_t)n; continue; }
            if (n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (!poll_for(fd, POLLOUT, 30000)) return false;
                continue;
            }
            if (n == MBEDTLS_ERR_SSL_WANT_READ) {
                // 罕见：handshake 期。读侧也准备好就继续写。
                if (!poll_for(fd, POLLIN, 30000)) return false;
                continue;
            }
            CAM_WARN("[tls] ssl_write failed: %s", tls_strerror(n).c_str());
            return false;
        }
        return true;
    }
    const char* p = (const char*)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!poll_for(fd, POLLOUT, 30000)) return false;
                continue;
            }
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

int ClientConn::read_some(void* buf, size_t len, int timeout_ms, size_t& got) {
    if (ssl) {
        // 先 poll 拿 POLLIN，避免在 mbedtls_ssl_read 上干等。剩余预算内循环 WANT_READ。
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) { got = 0; return 0; }
            int remaining = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (!poll_for(fd, POLLIN, remaining)) { got = 0; return 0; }
            int n = mbedtls_ssl_read(ssl, (unsigned char*)buf, len);
            if (n > 0) { got = (size_t)n; return 1; }
            if (n == 0) { got = 0; return -1; }  // close_notify
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            // 其他错误（含 peer close / 解密失败 / 超长 record）→ 关闭
            got = 0; return -1;
        }
    }
    struct pollfd pfd = {fd, POLLIN, 0};
    int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc < 0) { got = 0; return -1; }
    if (rc == 0) { got = 0; return 0; }   // 超时
    ssize_t n = ::recv(fd, buf, len, 0);
    if (n <= 0) { got = 0; return -1; }   // 关闭/错误
    got = (size_t)n;
    return 1;
}

void ClientConn::close() {
    if (ssl) {
        // 优雅关闭：发 close_notify alert（忽略错误，对端可能已断）
        mbedtls_ssl_close_notify(ssl);
        mbedtls_ssl_free(ssl);
        delete ssl;
        ssl = nullptr;
    }
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

}  // namespace capp
