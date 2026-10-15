// 路由表：注册、匹配、静态文件服务
//
// 由 capp/src/http_server.cpp 拆出来（声明都还在 capp/http_server.hpp，类没变）。
// 精确匹配（插入序）+ 参数路由 `/x/{id}`；静态文件服务在这里（SPA 的 index.html 兜底）。

#include "capp/http_server.hpp"

#include "capp/context.hpp"

#include <cctype>
#include <string>

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

namespace {

// 文本类资源预压缩后体积能降到约 1/4。构建期同时产出 `<file>.br` 与 `<file>.gz`
// 兄弟文件（见 frontend 的 gzip-assets 步骤），板上请求期**不做压缩**，只把预压缩
// 字节原样流出并打上对应 `Content-Encoding`。这是针对 AIC8800 WiFi TX 上限
// （~5–6KB/s，根因是驱动缺 host 端 TX 聚合，另立 PR）的传输层规避：317KB 的
// index.js → brotli ~80KB / gzip ~94KB，浏览器能在放弃（黑屏）之前把它取完。
bool compressible(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == "js" || ext == "mjs" || ext == "css" || ext == "html" ||
           ext == "htm" || ext == "json" || ext == "svg" || ext == "map" ||
           ext == "txt";
}

const char* cache_control_for(const std::string& rel) {
    // assets 已带内容哈希（index-<hash>.js），可永久缓存；index.html 等 no-cache，
    // 每次发版换哈希文件名 → 浏览器自动取新包，不再出现"改了看不到"。
    return rel.rfind("/assets/", 0) == 0 ? "public, max-age=86400" : "no-cache";
}

// 服务 `file`：按 Accept-Encoding 依次尝试 br、gzip 的预压缩兄弟文件，都没有再发
// 原始文件。找到并发完响应返回 true；文件不存在返回 false，交给调用方走 SPA/404。
bool serve_one(const std::string& file, const std::string& rel,
               const HttpRequest& req, HttpResponse& resp) {
    const std::string ae = req.header("accept-encoding");

    // 预压缩变体，按优先顺序（brotli 压得更小，gzip 兜底给不支持 br 的客户端）。
    struct Variant { const char* token; const char* suffix; const char* encoding; };
    static const Variant kVariants[] = {
        {"br", ".br", "br"},
        {"gzip", ".gz", "gzip"},
    };
    if (compressible(file) && !ae.empty()) {
        for (const Variant& v : kVariants) {
            if (ae.find(v.token) == std::string::npos) continue;
            std::string content;
            if (http_util::read_file(file + v.suffix, content)) {
                resp.status = 200;
                resp.body = std::move(content);
                resp.headers["Content-Type"] = http_util::mime_type(file);  // 原类型，非 .br/.gz
                resp.headers["Content-Encoding"] = v.encoding;
                resp.headers["Vary"] = "Accept-Encoding";
                resp.headers["Cache-Control"] = cache_control_for(rel);
                resp.headers["Access-Control-Allow-Origin"] = "*";
                return true;
            }
        }
    }

    std::string content;
    if (http_util::read_file(file, content)) {
        resp.status = 200;
        resp.body = std::move(content);
        resp.headers["Content-Type"] = http_util::mime_type(file);
        resp.headers["Cache-Control"] = cache_control_for(rel);
        // 只有客户端做过内容协商才标 Vary，纯请求保持与旧行为一致。
        if (!ae.empty()) resp.headers["Vary"] = "Accept-Encoding";
        resp.headers["Access-Control-Allow-Origin"] = "*";
        return true;
    }
    return false;
}

}  // namespace

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

    if (serve_one(file, rel, req, resp)) return true;

    // fallback: SPA 路由 → index.html（非 API 路径）
    if (rel.rfind("/api/", 0) != 0) {
        std::string idx = static_dir_ + "/" + index_file_;
        if (serve_one(idx, "/", req, resp)) return true;
    }
    resp.set_error("Not Found", 404);
    return true;
}

}  // namespace capp
