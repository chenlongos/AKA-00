// HTTP 报文：HttpRequest / HttpResponse / http_util
//
// 由 capp/src/http_server.cpp 拆出来（声明都还在 capp/http_server.hpp，类没变）。
// 请求解析后的读取辅助（header/query/json 取值）、响应的三种 set_*、以及 URL 解码、MIME、读文件这些小工具。

#include "capp/http_server.hpp"

#include <fstream>
#include <sstream>

namespace capp {


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

}  // namespace capp
