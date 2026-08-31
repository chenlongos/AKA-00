// capp/http_server.cpp

#include "capp/http_server.hpp"

#include <algorithm>
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

namespace capp {

// ═══════════════════════ ClientConn ═══════════════════════

bool ClientConn::write_all(const void* data, size_t len) {
    const char* p = (const char*)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

int ClientConn::read_some(void* buf, size_t len, int timeout_ms, size_t& got) {
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

void HttpServer::run() {
    if (listen_fd_ < 0) return;
    // 非阻塞 + poll 轮询：signal() 默认 SA_RESTART 会让 accept 永不返回，
    // 必须周期性检查 ctx_.shutdown 才能优雅退出（OTA 重启依赖 SIGTERM）
    int flags = fcntl(listen_fd_, F_GETFL, 0);
    fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

    while (!ctx_.shutdown) {
        struct pollfd pfd = {listen_fd_, POLLIN, 0};
        int rc = ::poll(&pfd, 1, 200);
        if (rc <= 0) continue;
        sockaddr_in client;
        socklen_t len = sizeof client;
        int fd = ::accept(listen_fd_, (sockaddr*)&client, &len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            CAM_WARN("[http] accept: %s", std::strerror(errno));
            continue;
        }
        std::thread([this, fd] { handle_connection(fd); }).detach();
    }
    ::close(listen_fd_);
    listen_fd_ = -1;
    CAM_INFO("[http] server stopped");
}

bool HttpServer::read_request(int fd, HttpRequest& req) {
    // 读头（≤64KB）
    std::string buf;
    char tmp[4096];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        struct pollfd pfd = {fd, POLLIN, 0};
        int rc = ::poll(&pfd, 1, 10000);
        if (rc <= 0) return false;
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
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
            ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) break;
            req.body.append(tmp, (size_t)n);
        }
        req.body.resize((size_t)cl);
    }
    return true;
}

void HttpServer::send_response(int fd, const HttpResponse& resp, const HttpRequest& req) {
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

    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

void HttpServer::handle_connection(int fd) {
    ClientConn conn;
    conn.fd = fd;

    HttpRequest req;
    if (!read_request(fd, req)) {
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
        send_response(fd, resp, req);
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
    send_response(fd, resp, req);
    conn.close();
}

}  // namespace capp
