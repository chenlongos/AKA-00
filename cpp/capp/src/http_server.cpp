// capp/http_server.cpp

#include "capp/http_server.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "capp/context.hpp"
#include "csrc/log.hpp"

// mbedTLS —— TLS 终止（HttpServer::listen_tls）。轻量、可静态链接到 riscv64 musl。
// 编译时通过 cpp/scripts/build-mbedtls.sh 生成 third_party/mbedtls/。
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace capp {

// ═══════════════════════ TLS helpers ═══════════════════════
//
// 自定义 BIO 把 mbedtls_ssl_* 的 I/O 重定向到 ClientConn 的 fd，并把 EAGAIN
// 翻译成 MBEDTLS_ERR_SSL_WANT_READ/WANT_WRITE，让上层 write_all / read_some
// 里的 poll 循环处理背压。SIGPIPE 已在 main.cpp 用 SIG_IGN 屏蔽，可直接 send()。

static int tsl_bio_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    ssize_t n = ::send(fd, buf, len, 0);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tsl_bio_recv(void* ctx, unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    ssize_t n = ::recv(fd, buf, len, 0);
    if (n > 0) return (int)n;
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

// 把 mbedTLS 错误码转成可读字符串（调试用）
static std::string tls_strerror(int code) {
    char buf[256];
    mbedtls_strerror(code, buf, sizeof buf);
    return std::string(buf);
}

// ═══════════════════════ ClientConn ═══════════════════════

// WANT_READ/WANT_WRITE 时的轮询辅助：成功返回 true，超时或硬错误返回 false。
static bool poll_for(int fd, short events, int timeout_ms) {
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

// ═══════════════════════ HttpRequest ═══════════════════════

std::string HttpRequest::header(const std::string& name) const {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    auto it = headers.find(key);
    return it == headers.end() ? "" : it->second;
}

std::string HttpRequest::query_param(const std::string& key, const std::string& def) const {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        std::string k = http_util::url_decode(eq == std::string::npos ? pair : pair.substr(0, eq));
        if (k == key) {
            return eq == std::string::npos ? "" : http_util::url_decode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return def;
}

csrc::Json HttpRequest::json() const {
    csrc::Json out;
    if (!csrc::Json::parse(body, out)) return csrc::Json();
    return out;
}

bool HttpRequest::is_json_body() const {
    std::string ct = header("content-type");
    return ct.find("application/json") != std::string::npos;
}

// ═══════════════════════ HttpResponse ═══════════════════════

void HttpResponse::set_json(const csrc::Json& j, int code) {
    status = code;
    body = j.dump(false);
    headers["Content-Type"] = "application/json";
    headers["Access-Control-Allow-Origin"] = "*";
    headers["Access-Control-Allow-Methods"] = "GET,POST,PUT,PATCH,DELETE,OPTIONS";
    headers["Access-Control-Allow-Headers"] = "Content-Type,Authorization";
    headers["Access-Control-Max-Age"] = "86400";
}

void HttpResponse::set_error(const std::string& msg, int code) {
    csrc::Json j;
    j["error"] = msg;
    set_json(j, code);
}

void HttpResponse::set_text(const std::string& text, const std::string& content_type) {
    status = 200;
    body = text;
    headers["Content-Type"] = content_type;
}

// ═══════════════════════ http_util ═══════════════════════

namespace http_util {

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back((char)((h << 4) | l));
                i += 2;
                continue;
            }
        } else if (s[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string mime_type(const std::string& path) {
    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "application/javascript";
    if (ext == ".css") return "text/css";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ttf") return "font/ttf";
    if (ext == ".webp") return "image/webp";
    if (ext == ".gif") return "image/gif";
    if (ext == ".mp4") return "video/mp4";
    if (ext == ".map") return "application/json";
    return "application/octet-stream";
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace http_util

// ═══════════════════════ Router ═══════════════════════

void Router::add(const std::string& method, const std::string& path, Handler h) {
    Entry e;
    e.method = method;
    e.path = path;
    e.handler = std::move(h);
    entries_.push_back(std::move(e));
}

void Router::add_param(const std::string& method, const std::string& pattern, Handler h) {
    Entry e;
    e.method = method;
    e.pattern = pattern;
    e.is_param = true;
    // 解析 {name} 段
    std::string cur;
    bool in_brace = false;
    for (char c : pattern) {
        if (c == '{') { in_brace = true; cur.clear(); }
        else if (c == '}') { in_brace = false; e.param_names.push_back(cur); }
        else if (in_brace) cur.push_back(c);
    }
    e.handler = std::move(h);
    entries_.push_back(std::move(e));
}

bool Router::dispatch(const std::string& method, const std::string& path,
                      const HttpRequest& req, HttpResponse& resp, ClientConn& conn, AppContext& ctx) {
    for (auto& e : entries_) {
        if (e.method != method) continue;
        if (e.is_param) {
            // 模板: /api/demo/download_progress/{task_id}
            size_t p1 = e.pattern.find('{');
            if (p1 == std::string::npos) continue;
            std::string prefix = e.pattern.substr(0, p1);
            std::string suffix = e.pattern.substr(e.pattern.find('}') + 1);
            if (path.rfind(prefix, 0) != 0) continue;
            if (path.size() < prefix.size() + suffix.size()) continue;
            std::string mid = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
            if (path.substr(path.size() - suffix.size()) != suffix) continue;
            // 用参数构造一个扩展请求（存入 headers 特殊 key 供 handler 读取）
            HttpRequest ext = req;
            ext.headers["__route_param"] = mid;
            e.handler(ext, resp, conn, ctx);
            return true;
        }
        if (e.path == path) {
            e.handler(req, resp, conn, ctx);
            return true;
        }
    }
    return false;
}

bool Router::serve_static(const HttpRequest& req, HttpResponse& resp) {
    if (static_dir_.empty()) return false;

    std::string rel = req.path;
    // 防目录穿越
    if (rel.find("..") != std::string::npos) {
        resp.set_error("forbidden", 403);
        return true;
    }
    std::string file = static_dir_ + rel;
    if (rel == "/" || rel.empty()) {
        file = static_dir_ + "/" + index_file_;
    }

    std::string content;
    if (http_util::read_file(file, content)) {
        resp.status = 200;
        resp.body = std::move(content);
        resp.headers["Content-Type"] = http_util::mime_type(file);
        // assets 已带内容哈希（index-<hash>.js），可永久缓存；index.html 等 no-cache，
        // 每次发版换哈希文件名 → 浏览器自动取新包，不再出现"改了看不到"。
        resp.headers["Cache-Control"] = rel.rfind("/assets/", 0) == 0 ? "public, max-age=86400" : "no-cache";
        resp.headers["Access-Control-Allow-Origin"] = "*";
        return true;
    }

    // fallback: SPA 路由 → index.html（非 API 路径）
    if (rel.rfind("/api/", 0) != 0) {
        std::string idx = static_dir_ + "/" + index_file_;
        if (http_util::read_file(idx, content)) {
            resp.status = 200;
            resp.body = std::move(content);
            resp.headers["Content-Type"] = "text/html; charset=utf-8";
            resp.headers["Access-Control-Allow-Origin"] = "*";
            return true;
        }
    }
    resp.set_error("Not Found", 404);
    return true;
}

// ═══════════════════════ HttpServer ═══════════════════════

bool HttpServer::listen(int port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
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

    // bind + listen（与 listen() 同模式）
    tls_listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
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
    if (cl > 0 && cl <= (1 << 20)) {
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

void HttpServer::send_response(ClientConn& conn, const HttpResponse& resp, const HttpRequest& req) {
    std::string status_text = resp.status_text;
    if (status_text.empty()) {
        switch (resp.status) {
            case 200: status_text = "OK"; break;
            case 204: status_text = "No Content"; break;
            case 400: status_text = "Bad Request"; break;
            case 403: status_text = "Forbidden"; break;
            case 404: status_text = "Not Found"; break;
            case 408: status_text = "Request Timeout"; break;
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

    // CORS 预检
    if (req.method == "OPTIONS") {
        HttpResponse resp;
        resp.status = 204;
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
