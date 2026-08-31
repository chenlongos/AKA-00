// csrc/http_client.cpp

#include "csrc/http_client.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

#include "csrc/log.hpp"
#include "csrc/system_utils.hpp"

namespace csrc {

bool Url::parse(const std::string& url, Url& out) {
    // scheme://host[:port]/path
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    out.scheme = url.substr(0, scheme_end);
    size_t host_start = scheme_end + 3;
    size_t path_start = url.find('/', host_start);
    std::string host_port = path_start == std::string::npos
                                ? url.substr(host_start)
                                : url.substr(host_start, path_start - host_start);
    out.path = path_start == std::string::npos ? "/" : url.substr(path_start);

    out.port = out.scheme == "https" ? 443 : 80;
    size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        out.host = host_port.substr(0, colon);
        out.port = atoi(host_port.substr(colon + 1).c_str());
        if (out.port <= 0) out.port = out.scheme == "https" ? 443 : 80;
    } else {
        out.host = host_port;
    }
    if (out.host.empty()) return false;
    return true;
}

namespace {

int tcp_connect(const std::string& host, int port, int timeout_sec) {
    struct addrinfo hints, *res = nullptr;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || !res) {
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        // 非阻塞 connect + select 超时
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        if (rc != 0) {
            struct timeval tv = {timeout_sec, 0};
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            rc = select(fd + 1, nullptr, &wfds, nullptr, &tv);
            if (rc <= 0) { close(fd); fd = -1; continue; }
            int err = 0;
            socklen_t elen = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err != 0) { close(fd); fd = -1; continue; }
        }
        fcntl(fd, F_SETFL, flags);
        break;
    }
    freeaddrinfo(res);
    return fd;
}

bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

/// 读响应头直到 \r\n\r\n，返回 (status, headers)。
/// leftover 保存与头同包到达的 body 字节（必须交给 body 读取，否则会丢数据）。
bool read_headers(int fd, int& status, std::map<std::string, std::string>& headers,
                  std::string& header_block, std::string& leftover) {
    std::string buf;
    char tmp[4096];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
        if (buf.size() > 1 << 20) return false;
    }
    size_t hdr_end = buf.find("\r\n\r\n");
    header_block = buf.substr(0, hdr_end);
    leftover = buf.substr(hdr_end + 4);

    // 状态行
    size_t sp1 = header_block.find(' ');
    size_t sp2 = header_block.find(' ', sp1 + 1);
    if (sp1 == std::string::npos) return false;
    status = atoi(header_block.substr(sp1 + 1, sp2 - sp1 - 1).c_str());

    // 头字段
    size_t pos = header_block.find("\r\n");
    while (pos != std::string::npos && pos + 2 < header_block.size()) {
        size_t line_end = header_block.find("\r\n", pos + 2);
        if (line_end == std::string::npos) line_end = header_block.size();
        std::string line = header_block.substr(pos + 2, line_end - pos - 2);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            // trim v
            size_t b = v.find_first_not_of(" \t");
            if (b != std::string::npos) v = v.substr(b);
            for (auto& c : k) c = (char)tolower(c);
            headers[k] = v;
        }
        pos = line_end;
    }
    return true;
}

std::string read_body(int fd, long long content_length, const std::string& leftover,
                      int timeout_sec) {
    std::string body = leftover;
    if (content_length < 0) {
        // chunked 或未知：读到 EOF（超时兜底）
        char tmp[16384];
        fd_set rfds;
        struct timeval tv = {timeout_sec, 0};
        for (;;) {
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            int rc = select(fd + 1, &rfds, nullptr, nullptr, &tv);
            if (rc <= 0) break;
            ssize_t n = recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) break;
            body.append(tmp, (size_t)n);
            tv = {timeout_sec, 0};
        }
        return body;
    }
    body.reserve((size_t)content_length);
    while ((long long)body.size() < content_length) {
        char tmp[16384];
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        body.append(tmp, (size_t)n);
    }
    return body;
}

HttpResult http_request_raw(const std::string& request, const std::string& url,
                            int timeout_sec) {
    HttpResult out;
    Url u;
    if (!Url::parse(url, u)) {
        out.error = "invalid url: " + url;
        return out;
    }
    if (u.scheme == "https") {
        out.error = "https not supported by socket client (use curl fallback)";
        return out;
    }
    int fd = tcp_connect(u.host, u.port, timeout_sec);
    if (fd < 0) {
        out.error = "connect " + u.host + ":" + std::to_string(u.port) + " failed";
        return out;
    }
    if (!send_all(fd, request)) {
        out.error = "send failed";
        close(fd);
        return out;
    }
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string hdr, leftover;
    if (!read_headers(fd, status, headers, hdr, leftover)) {
        out.error = "read headers failed";
        close(fd);
        return out;
    }
    long long cl = -1;
    auto it = headers.find("content-length");
    if (it != headers.end()) cl = atoll(it->second.c_str());
    out.body = read_body(fd, cl, leftover, timeout_sec);
    out.status = status;
    out.ok = status >= 200 && status < 300;
    close(fd);
    return out;
}

}  // namespace

HttpResult http_get(const std::string& url, int timeout_sec) {
    Url u;
    if (!Url::parse(url, u)) return {false, 0, "", "invalid url"};
    if (u.scheme == "https") {
        // curl 兜底
        std::string out = exec_output("curl -sS -m " + std::to_string(timeout_sec) +
                                      " \"" + url + "\" 2>&1");
        HttpResult r;
        r.ok = !out.empty();
        r.body = out;
        if (!r.ok) r.error = "curl failed";
        return r;
    }
    std::string req = "GET " + u.path + " HTTP/1.1\r\n"
                      "Host: " + u.host + "\r\n"
                      "User-Agent: AKA-00-CPP/1.0\r\n"
                      "Accept: application/json\r\n"
                      "Connection: close\r\n\r\n";
    return http_request_raw(req, url, timeout_sec);
}

HttpResult http_post_json(const std::string& url, const std::string& json_body, int timeout_sec) {
    Url u;
    if (!Url::parse(url, u)) return {false, 0, "", "invalid url"};
    if (u.scheme == "https") {
        // 写临时文件再 curl -d @file，避免 shell 转义
        std::string tmp = "/tmp/aka-post-body.json";
        {
            std::ofstream f(tmp, std::ios::binary);
            f << json_body;
        }
        std::string out = exec_output("curl -sS -m " + std::to_string(timeout_sec) +
                                      " -X POST -H 'Content-Type: application/json' "
                                      " --data-binary @" + tmp + " \"" + url + "\" 2>&1");
        HttpResult r;
        r.ok = !out.empty();
        r.body = out;
        if (!r.ok) r.error = "curl failed";
        return r;
    }
    std::string req = "POST " + u.path + " HTTP/1.1\r\n"
                      "Host: " + u.host + "\r\n"
                      "User-Agent: AKA-00-CPP/1.0\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(json_body.size()) + "\r\n"
                      "Connection: close\r\n\r\n" + json_body;
    return http_request_raw(req, url, timeout_sec);
}

HttpResult http_download(const std::string& url, const std::string& dest_path,
                         std::function<void(int)> progress_cb, int timeout_sec) {
    Url u;
    if (!Url::parse(url, u)) return {false, 0, "", "invalid url"};

    if (u.scheme == "https") {
        // curl 下载（无精细进度，完成时回调 100）
        std::string cmd = "curl -sS -m " + std::to_string(timeout_sec) +
                          " -o \"" + dest_path + "\" \"" + url + "\" 2>&1";
        std::string err_out = exec_output(cmd);
        if (!err_out.empty()) {
            CAM_WARN("[http] curl download failed: %s", err_out.c_str());
            return {false, 0, "", err_out};
        }
        if (progress_cb) progress_cb(100);
        return {true, 200, "", ""};
    }

    // socket 下载（带进度）
    int fd = tcp_connect(u.host, u.port, timeout_sec);
    if (fd < 0) return {false, 0, "", "connect failed"};
    std::string req = "GET " + u.path + " HTTP/1.1\r\n"
                      "Host: " + u.host + "\r\n"
                      "User-Agent: AKA-00-CPP/1.0\r\n"
                      "Connection: close\r\n\r\n";
    if (!send_all(fd, req)) {
        close(fd);
        return {false, 0, "", "send failed"};
    }
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string hdr, leftover;
    if (!read_headers(fd, status, headers, hdr, leftover)) {
        close(fd);
        return {false, 0, "", "read headers failed"};
    }
    long long cl = -1;
    auto it = headers.find("content-length");
    if (it != headers.end()) cl = atoll(it->second.c_str());

    std::ofstream f(dest_path, std::ios::binary | std::ios::trunc);
    if (!f) {
        close(fd);
        return {false, 0, "", "cannot open " + dest_path};
    }
    long long downloaded = 0;
    // 先写与头同包到达的 body 字节
    if (!leftover.empty()) {
        f.write(leftover.data(), (std::streamsize)leftover.size());
        downloaded += (long long)leftover.size();
    }
    char tmp[16384];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {timeout_sec, 0};
        int rc = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (rc <= 0) break;
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        f.write(tmp, n);
        downloaded += n;
        if (cl > 0 && progress_cb) {
            int pct = (int)(downloaded * 100 / cl);
            if (pct > 99) pct = 99;
            progress_cb(pct);
        }
    }
    f.close();
    close(fd);
    if (cl > 0 && downloaded < cl) {
        return {false, 0, "", "download incomplete"};
    }
    if (progress_cb) progress_cb(100);
    return {true, status, "", ""};
}

}  // namespace csrc
