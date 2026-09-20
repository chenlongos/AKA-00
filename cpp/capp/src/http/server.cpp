// HttpServer：监听（含 TLS）、accept 循环、读请求、写响应
//
// 由 capp/src/http_server.cpp 拆出来（声明都还在 capp/http_server.hpp，类没变）。
// 一个连接一个线程；请求体上限与超时都在这里。

#include "capp/http_server.hpp"

#include "capp/context.hpp"   // AppContext（handle_connection 里要用它的完整类型）

#include "csrc/log.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netinet/in.h>
#include <poll.h>
#include <thread>   // 一个连接一个线程
#include <sys/socket.h>
#include <unistd.h>
#include "http_internal.hpp"

namespace capp {


// ═══════════════════════ HttpServer ═══════════════════════

bool HttpServer::listen(int port) {
    // SOCK_CLOEXEC：监听 socket 不能被子进程继承。
    // 板上踩过：capp 会 fork/exec 去拉 wpa_supplicant（ensure_wpa_env），子进程
    // 继承了监听 socket 后 daemon 化（-B）长期持有 —— capp 一旦重启，端口仍被那个
    // 无关进程占着，新实例 bind 失败退出，init.sh 每 2 秒重启一次变成死循环，
    // 表现是"整个服务再也起不来，只能重启板子"。
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        CAM_ERROR("[http] socket: %s", std::strerror(errno));
        return false;
    }
    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (::bind(listen_fd_, (sockaddr*)&addr, sizeof addr) < 0) {
        CAM_ERROR("[http] bind :%d failed: %s", port, std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 16) < 0) {
        CAM_ERROR("[http] listen: %s", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    CAM_INFO("[http] listening on 0.0.0.0:%d", port);
    return true;
}

bool HttpServer::listen_tls(int port, const std::string& cert_path, const std::string& key_path) {
    if (port <= 0) return false;

    // 一次性加载 cert / key / entropy+ctr_drbg（init 后只读，多线程共享安全）
    auto* cfg = new mbedtls_ssl_config;
    auto* cert = new mbedtls_x509_crt;
    auto* key = new mbedtls_pk_context;
    auto* entropy = new mbedtls_entropy_context;
    auto* drbg = new mbedtls_ctr_drbg_context;
    mbedtls_ssl_config_init(cfg);
    mbedtls_x509_crt_init(cert);
    mbedtls_pk_init(key);
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(drbg);

    // CTR-DRBG 比直接用 entropy_func 更可靠（避免某些平台的 NV seed 失败）。
    // 用 mbedtls_entropy_func 给 drbg 做 seed。
    int rc = mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, entropy, nullptr, 0);
    if (rc != 0) {
        CAM_ERROR("[tls] ctr_drbg_seed: %s", tls_strerror(rc).c_str());
        mbedtls_ctr_drbg_free(drbg); delete drbg;
        mbedtls_entropy_free(entropy); delete entropy;
        mbedtls_x509_crt_free(cert); delete cert;
        mbedtls_pk_free(key); delete key;
        mbedtls_ssl_config_free(cfg); delete cfg;
        return false;
    }

    rc = mbedtls_x509_crt_parse_file(cert, cert_path.c_str());
    if (rc != 0) {
        CAM_ERROR("[tls] parse cert %s failed: %s", cert_path.c_str(), tls_strerror(rc).c_str());
        mbedtls_ctr_drbg_free(drbg); delete drbg;
        mbedtls_entropy_free(entropy); delete entropy;
        mbedtls_x509_crt_free(cert); delete cert;
        mbedtls_pk_free(key); delete key;
        mbedtls_ssl_config_free(cfg); delete cfg;
        return false;
    }
    // key 未加密（openssl -nodes 生成），ctr_drbg_random 满足 f_rng 签名
    rc = mbedtls_pk_parse_keyfile(key, key_path.c_str(), nullptr,
                                  mbedtls_ctr_drbg_random, drbg);
    if (rc != 0) {
        CAM_ERROR("[tls] parse key %s failed: %s", key_path.c_str(), tls_strerror(rc).c_str());
        mbedtls_ctr_drbg_free(drbg); delete drbg;
        mbedtls_entropy_free(entropy); delete entropy;
        mbedtls_x509_crt_free(cert); delete cert;
        mbedtls_pk_free(key); delete key;
        mbedtls_ssl_config_free(cfg); delete cfg;
        return false;
    }
    rc = mbedtls_ssl_config_defaults(cfg, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        CAM_ERROR("[tls] ssl_config_defaults: %s", tls_strerror(rc).c_str());
        mbedtls_ctr_drbg_free(drbg); delete drbg;
        mbedtls_entropy_free(entropy); delete entropy;
        mbedtls_x509_crt_free(cert); delete cert;
        mbedtls_pk_free(key); delete key;
        mbedtls_ssl_config_free(cfg); delete cfg;
        return false;
    }
    mbedtls_ssl_conf_authmode(cfg, MBEDTLS_SSL_VERIFY_NONE);  // 自签证书，客户端跳过校验
    mbedtls_ssl_conf_rng(cfg, mbedtls_ctr_drbg_random, drbg);
    rc = mbedtls_ssl_conf_own_cert(cfg, cert, key);
    if (rc != 0) {
        CAM_ERROR("[tls] conf_own_cert: %s", tls_strerror(rc).c_str());
        mbedtls_ctr_drbg_free(drbg); delete drbg;
        mbedtls_entropy_free(entropy); delete entropy;
        mbedtls_x509_crt_free(cert); delete cert;
        mbedtls_pk_free(key); delete key;
        mbedtls_ssl_config_free(cfg); delete cfg;
        return false;
    }

    // bind + listen（与 listen() 同模式；SOCK_CLOEXEC 的原因见 listen() 里的注释）
    tls_listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (tls_listen_fd_ < 0) {
        CAM_ERROR("[tls] socket: %s", std::strerror(errno));
        mbedtls_ctr_drbg_free(drbg); delete drbg;
        mbedtls_entropy_free(entropy); delete entropy;
        mbedtls_x509_crt_free(cert); delete cert;
        mbedtls_pk_free(key); delete key;
        mbedtls_ssl_config_free(cfg); delete cfg;
        return false;
    }
    int yes = 1;
    setsockopt(tls_listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (::bind(tls_listen_fd_, (sockaddr*)&addr, sizeof addr) < 0) {
        CAM_ERROR("[tls] bind :%d failed: %s", port, std::strerror(errno));
        ::close(tls_listen_fd_); tls_listen_fd_ = -1;
        mbedtls_ctr_drbg_free(drbg); delete drbg;
        mbedtls_entropy_free(entropy); delete entropy;
        mbedtls_x509_crt_free(cert); delete cert;
        mbedtls_pk_free(key); delete key;
        mbedtls_ssl_config_free(cfg); delete cfg;
        return false;
    }
    if (::listen(tls_listen_fd_, 16) < 0) {
        CAM_ERROR("[tls] listen: %s", std::strerror(errno));
        ::close(tls_listen_fd_); tls_listen_fd_ = -1;
        mbedtls_ctr_drbg_free(drbg); delete drbg;
        mbedtls_entropy_free(entropy); delete entropy;
        mbedtls_x509_crt_free(cert); delete cert;
        mbedtls_pk_free(key); delete key;
        mbedtls_ssl_config_free(cfg); delete cfg;
        return false;
    }

    // 全部成功 → 把所有权交给 HttpServer 成员
    tls_ssl_cfg_ = cfg;
    tls_cert_    = cert;
    tls_key_     = key;
    tls_entropy_ = entropy;
    tls_drbg_    = drbg;
    tls_ready_   = true;

    CAM_INFO("[tls] listening on 0.0.0.0:%d (cert=%s, key=%s)",
             port, cert_path.c_str(), key_path.c_str());
    return true;
}

void HttpServer::run() {
    if (listen_fd_ < 0 && !tls_ready_) return;
    // 两个 listen socket 都设非阻塞，poll 一起等（signal 默认 SA_RESTART
    // 会让 accept 永不返回，必须周期性检查 ctx_.shutdown）
    if (listen_fd_ >= 0) {
        int flags = fcntl(listen_fd_, F_GETFL, 0);
        fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);
    }
    if (tls_listen_fd_ >= 0) {
        int flags = fcntl(tls_listen_fd_, F_GETFL, 0);
        fcntl(tls_listen_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    struct pollfd pfds[2];
    int n_fds = 0;
    if (listen_fd_ >= 0) pfds[n_fds++] = {listen_fd_, POLLIN, 0};
    if (tls_listen_fd_ >= 0) pfds[n_fds++] = {tls_listen_fd_, POLLIN, 0};

    while (!ctx_.shutdown) {
        int rc = ::poll(pfds, n_fds, 200);
        if (rc <= 0) continue;

        for (int i = 0; i < n_fds; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            sockaddr_in client;
            socklen_t len = sizeof client;
            int fd = ::accept(pfds[i].fd, (sockaddr*)&client, &len);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                CAM_WARN("[http] accept: %s", std::strerror(errno));
                continue;
            }
            // 连接 fd 同样不要漏给子进程（否则 fork 出去的 daemon 会替客户端
            // 一直握着这条连接，对端收不到 RST）
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            bool is_tls = (pfds[i].fd == tls_listen_fd_);
            std::thread([this, fd, is_tls] { handle_connection(fd, is_tls); }).detach();
        }
    }
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    if (tls_listen_fd_ >= 0) { ::close(tls_listen_fd_); tls_listen_fd_ = -1; }
    // 释放 TLS 全局资源
    if (tls_ready_) {
        mbedtls_ssl_config_free((mbedtls_ssl_config*)tls_ssl_cfg_);
        mbedtls_x509_crt_free((mbedtls_x509_crt*)tls_cert_);
        mbedtls_pk_free((mbedtls_pk_context*)tls_key_);
        mbedtls_ctr_drbg_free((mbedtls_ctr_drbg_context*)tls_drbg_);
        mbedtls_entropy_free((mbedtls_entropy_context*)tls_entropy_);
        delete (mbedtls_ssl_config*)tls_ssl_cfg_;
        delete (mbedtls_x509_crt*)tls_cert_;
        delete (mbedtls_pk_context*)tls_key_;
        delete (mbedtls_ctr_drbg_context*)tls_drbg_;
        delete (mbedtls_entropy_context*)tls_entropy_;
        tls_ssl_cfg_ = nullptr; tls_cert_ = nullptr; tls_key_ = nullptr;
        tls_drbg_ = nullptr; tls_entropy_ = nullptr;
        tls_ready_ = false;
    }
    CAM_INFO("[http] server stopped");
}

bool HttpServer::read_request(ClientConn& conn, HttpRequest& req) {
    // 读头（≤64KB）。走 ClientConn::read_some → 自动适配 TLS。
    std::string buf;
    char tmp[4096];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        size_t got = 0;
        int rc = conn.read_some(tmp, sizeof tmp, 10000, got);
        if (rc <= 0) return false;
        buf.append(tmp, got);
        if (buf.size() > 1 << 16) return false;
    }
    size_t hdr_end = buf.find("\r\n\r\n");
    std::string head = buf.substr(0, hdr_end);

    // 请求行
    size_t line_end = head.find("\r\n");
    std::string line = line_end == std::string::npos ? head : head.substr(0, line_end);
    std::istringstream iss(line);
    iss >> req.method >> req.raw_path;
    if (req.method.empty() || req.raw_path.empty()) return false;

    // path + query
    size_t qpos = req.raw_path.find('?');
    if (qpos == std::string::npos) {
        req.path = http_util::url_decode(req.raw_path);
    } else {
        req.path = http_util::url_decode(req.raw_path.substr(0, qpos));
        req.query = req.raw_path.substr(qpos + 1);
    }

    // headers
    size_t pos = line_end;
    while (pos != std::string::npos && pos + 2 < head.size()) {
        size_t e = head.find("\r\n", pos + 2);
        std::string h = head.substr(pos + 2, (e == std::string::npos ? head.size() : e) - pos - 2);
        size_t colon = h.find(':');
        if (colon != std::string::npos) {
            std::string k = h.substr(0, colon);
            std::string v = h.substr(colon + 1);
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            size_t b = v.find_first_not_of(" \t");
            if (b != std::string::npos) v = v.substr(b);
            req.headers[k] = v;
        }
        if (e == std::string::npos) break;
        pos = e;
    }

    // body
    long long cl = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) cl = atoll(it->second.c_str());
    // curl 等客户端对 >1KB 的体会先发 `Expect: 100-continue`，等这个握手才发体 ——
    // 不回它就一直等（现象是路由收到空 body）。必须先回 100 Continue 再读。
    auto exp = req.headers.find("expect");
    if (cl > 0 && exp != req.headers.end() &&
        exp->second.find("100-continue") != std::string::npos) {
        conn.write_all("HTTP/1.1 100 Continue\r\n\r\n");
    }
    if (cl > 0 && cl <= kMaxRequestBody) {
        req.body = buf.substr(hdr_end + 4);
        while ((long long)req.body.size() < cl) {
            size_t got = 0;
            int rc = conn.read_some(tmp, sizeof tmp, 10000, got);
            if (rc <= 0) break;
            req.body.append(tmp, got);
        }
        req.body.resize((size_t)cl);
    }
    return true;
}

void HttpServer::send_response(ClientConn& conn, const HttpResponse& resp, const HttpRequest& /*req*/) {
    std::string status_text = resp.status_text;
    if (status_text.empty()) {
        switch (resp.status) {
            case 200: status_text = "OK"; break;
            case 204: status_text = "No Content"; break;
            case 400: status_text = "Bad Request"; break;
            case 403: status_text = "Forbidden"; break;
            case 404: status_text = "Not Found"; break;
            case 408: status_text = "Request Timeout"; break;
            case 413: status_text = "Payload Too Large"; break;
            case 409: status_text = "Conflict"; break;
            case 500: status_text = "Internal Server Error"; break;
            case 502: status_text = "Bad Gateway"; break;
            case 503: status_text = "Service Unavailable"; break;
            default: status_text = "OK"; break;
        }
    }
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + status_text + "\r\n";
    for (auto& kv : resp.headers) {
        out += kv.first + ": " + kv.second + "\r\n";
    }
    if (resp.headers.find("Content-Length") == resp.headers.end()) {
        out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    }
    out += "Connection: close\r\n\r\n";
    out += resp.body;

    conn.write_all(out);  // 自动适配 TLS（重试 WANT_*）
}

void HttpServer::handle_connection(int fd, bool is_tls) {
    // TLS 握手前 fd 设非阻塞，mbedtls_ssl_handshake 才能正确返回 WANT_*
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    ClientConn conn;
    conn.fd = fd;

    if (is_tls) {
        // 分配连接级 SSL 上下文（共享 cfg；每个连接独立 session）
        auto* ssl = new mbedtls_ssl_context;
        mbedtls_ssl_init(ssl);
        int rc = mbedtls_ssl_setup(ssl, (mbedtls_ssl_config*)tls_ssl_cfg_);
        if (rc != 0) {
            CAM_WARN("[tls] ssl_setup: %s", tls_strerror(rc).c_str());
            mbedtls_ssl_free(ssl);
            delete ssl;
            conn.close();
            return;
        }
        // 自定义 BIO：读写都走 fd，EAGAIN → WANT_*，让上层 poll 处理
        mbedtls_ssl_set_bio(ssl, &conn.fd, tsl_bio_send, tsl_bio_recv, nullptr);

        // 握手：WANT_* 时 poll 对应方向再重试；15s 总超时
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        for (;;) {
            rc = mbedtls_ssl_handshake(ssl);
            if (rc == 0) break;  // 成功
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) break;  // 协议错
            if (std::chrono::steady_clock::now() >= deadline) { rc = -1; break; }
            short ev = (rc == MBEDTLS_ERR_SSL_WANT_READ) ? POLLIN : POLLOUT;
            if (!poll_for(fd, ev, 5000)) { rc = -1; break; }
            // loop: 再调一次 handshake
        }
        if (rc != 0) {
            CAM_WARN("[tls] handshake failed: %s", tls_strerror(rc).c_str());
            mbedtls_ssl_free(ssl);
            delete ssl;
            conn.close();
            return;
        }
        conn.ssl = ssl;
    }

    HttpRequest req;
    if (!read_request(conn, req)) {
        conn.close();
        return;
    }

    // CORS 预检：回 200（不是 204）—— 训练平台直传模型那份契约写的就是 200，
    // 虽然浏览器预检不看状态码，但按契约来省得对方以为哪里不对。
    if (req.method == "OPTIONS") {
        HttpResponse resp;
        resp.status = 200;
        resp.headers["Access-Control-Allow-Origin"] = "*";
        resp.headers["Access-Control-Allow-Methods"] = "GET,POST,PUT,PATCH,DELETE,OPTIONS";
        resp.headers["Access-Control-Allow-Headers"] = "Content-Type,Authorization";
        resp.headers["Access-Control-Max-Age"] = "86400";
        resp.headers["Content-Length"] = "0";
        send_response(conn, resp, req);
        conn.close();
        return;
    }

    HttpResponse resp;
    bool matched = router_.dispatch(req.method, req.path, req, resp, conn, ctx_);

    if (!matched) {
        // 静态文件 / 404
        if (req.method == "GET" || req.method == "HEAD") {
            router_.serve_static(req, resp);
        } else {
            resp.set_error("Not Found", 404);
        }
    }

    if (resp.stream) {
        // handler 已接管（MJPEG / WebSocket），连接由 handler 自行关闭
        return;
    }
    send_response(conn, resp, req);
    conn.close();
}

}  // namespace capp
