// csrc/http_client.hpp — 极简 HTTP 客户端（socket 实现；https 走 curl 兜底）
//
// 用途：OTA 版本检查/下载、状态上报、demo 模型下载。
//   - http://  直接用 socket 实现（含下载进度回调）
//   - https:// 板上无 TLS 库，走 `curl -sS`（系统需装 curl；不可用时返回错误）
//
// 返回: HttpResult { ok, status, body, error }

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace csrc {

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

struct Url {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path;   // 含 query

    static bool parse(const std::string& url, Url& out);
    std::string str() const { return scheme + "://" + host + path; }
};

/// GET，返回响应体
HttpResult http_get(const std::string& url, int timeout_sec = 10);

/// POST JSON body
HttpResult http_post_json(const std::string& url, const std::string& json_body, int timeout_sec = 10);

/// 下载到文件。progress_cb(percent) 可选；percent -1 表示未知总长。
HttpResult http_download(const std::string& url, const std::string& dest_path,
                         std::function<void(int)> progress_cb = nullptr,
                         int timeout_sec = 60);

}  // namespace csrc
