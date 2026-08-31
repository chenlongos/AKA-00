// capp/http_server.hpp — 极简 HTTP 服务器（自研，POSIX socket + 线程）
//
// 设计（针对单核 RISC-V 板子，量级为几个浏览器客户端）：
//   - accept 主循环 + 每连接一个线程（detached），共享 AppContext
//   - 解析: 请求行 + 头 + Content-Length body，query 参数 URL 解码
//   - 路由: 精确 path+method 匹配，另支持 /api/demo/download_progress/{id} 这类参数路由
//   - 静态文件: static 目录 + index.html fallback（非 /api 路径）
//   - CORS: 所有响应自动加 Access-Control-Allow-Origin 等；OPTIONS 预检 → 204
//   - 流式响应（MJPEG / WebSocket）: handler 直接写 ClientConn 并置 taken_over=true

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "csrc/json.hpp"

namespace capp {

struct AppContext;  // 定义见 context.hpp

struct ClientConn {
    int fd = -1;

    bool write_all(const void* data, size_t len);
    bool write_all(const std::string& s) { return write_all(s.data(), s.size()); }
    /// 读取原始字节（websocket/流式用）。
    /// 返回 1=读到数据(got>0) / 0=超时无数据 / -1=错误或连接关闭。
    int read_some(void* buf, size_t len, int timeout_ms, size_t& got);
    void close();
};

struct HttpRequest {
    std::string method;
    std::string path;         // 已解码，无 query
    std::string raw_path;     // 原始路径（含 query）
    std::string query;        // 原始 query 字符串
    std::map<std::string, std::string> headers;  // key 小写
    std::string body;

    std::string header(const std::string& name) const;
    /// URL 解码的 query 参数
    std::string query_param(const std::string& key, const std::string& def = "") const;
    /// 解析 body 为 JSON（失败返回空 Json）
    csrc::Json json() const;
    /// 是否 JSON 请求体（Content-Type 含 application/json）
    bool is_json_body() const;
};

struct HttpResponse {
    int status = 200;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;
    bool stream = false;      // true → handler 已接管连接（MJPEG/WS），服务器不再写响应

    void set_json(const csrc::Json& j, int code = 200);
    void set_error(const std::string& msg, int code = 400);
    void set_text(const std::string& text, const std::string& content_type = "text/plain; charset=utf-8");
};

/// 路由表 + 分发
class Router {
public:
    using Handler = std::function<void(const HttpRequest&, HttpResponse&, ClientConn&, AppContext&)>;

    void add(const std::string& method, const std::string& path, Handler h);
    /// 参数路由: path 含 {name} 段，如 /api/demo/download_progress/{task_id}
    void add_param(const std::string& method, const std::string& pattern, Handler h);
    /// 分发。返回 false = 未匹配（调用方走静态文件/404）
    bool dispatch(const std::string& method, const std::string& path,
                  const HttpRequest& req, HttpResponse& resp, ClientConn& conn, AppContext& ctx);

    void set_static_dir(const std::string& dir) { static_dir_ = dir; }
    void set_index_file(const std::string& f) { index_file_ = f; }

    /// 静态文件服务（返回 true 表示已写入响应）。供 fallback 使用。
    bool serve_static(const HttpRequest& req, HttpResponse& resp);

private:
    struct Entry {
        std::string method;
        std::string path;        // 精确
        std::string pattern;     // 参数路由模板
        std::vector<std::string> param_names;
        bool is_param = false;
        Handler handler;
    };
    std::vector<Entry> entries_;
    std::string static_dir_;
    std::string index_file_;
};

class HttpServer {
public:
    explicit HttpServer(AppContext& ctx) : ctx_(ctx) {}

    bool listen(int port);
    void run();   // 阻塞 accept 循环

    Router& router() { return router_; }

private:
    void handle_connection(int fd);
    bool read_request(int fd, HttpRequest& req);
    void send_response(int fd, const HttpResponse& resp, const HttpRequest& req);

    AppContext& ctx_;
    Router router_;
    int listen_fd_ = -1;
};

/// 工具函数（共享给路由实现）
namespace http_util {
std::string url_decode(const std::string& s);
std::string mime_type(const std::string& path);
bool read_file(const std::string& path, std::string& out);
}  // namespace http_util

}  // namespace capp
