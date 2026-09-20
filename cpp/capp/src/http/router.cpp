// 路由表：注册、匹配、静态文件服务
//
// 由 capp/src/http_server.cpp 拆出来（声明都还在 capp/http_server.hpp，类没变）。
// 精确匹配（插入序）+ 参数路由 `/x/{id}`；静态文件服务在这里（SPA 的 index.html 兜底）。

#include "capp/http_server.hpp"

#include "capp/context.hpp"

namespace capp {


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

}  // namespace capp
