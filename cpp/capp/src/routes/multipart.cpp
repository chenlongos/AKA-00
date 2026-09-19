// multipart/form-data 解析
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 供 models.cpp（传模型）与 ota.cpp（传固件）共用。

#include "routes_internal.hpp"

#include <cctype>

namespace capp {
namespace routes {

// multipart/form-data 解析（upload_model / OTA update / 训练平台直传模型 用）
//
// 一个 part 的三样东西：字段名 name、文件名 filename（文件字段才有）、内容 content。
// 注意**不能只取第一个 part**：训练平台的表单是 file + name 两个字段，先来哪个不确定。

std::vector<MultipartPart> parse_multipart(const std::string& body,
                                           const std::string& content_type) {
    std::vector<MultipartPart> out;
    size_t bpos = content_type.find("boundary=");
    if (bpos == std::string::npos) return out;
    std::string boundary = content_type.substr(bpos + 9);
    if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"') {
        boundary = boundary.substr(1, boundary.size() - 2);
    }
    const size_t semi = boundary.find(';');   // 有的客户端会写 boundary=xxx; charset=...
    if (semi != std::string::npos) boundary = boundary.substr(0, semi);
    if (boundary.empty()) return out;

    const std::string delim = "--" + boundary;
    size_t cursor = 0;
    while (true) {
        const size_t b = body.find(delim, cursor);
        if (b == std::string::npos) break;
        size_t after = b + delim.size();
        if (body.compare(after, 2, "--") == 0) break;      // 收尾的 --boundary--
        if (body.compare(after, 2, "\r\n") == 0) after += 2;
        const size_t hdr_end = body.find("\r\n\r\n", after);
        if (hdr_end == std::string::npos) break;
        const std::string headers = body.substr(after, hdr_end - after);
        const size_t data_start = hdr_end + 4;
        const size_t next = body.find(delim, data_start);
        size_t data_end = (next == std::string::npos) ? body.size() : next;
        if (data_end >= 2 && body.compare(data_end - 2, 2, "\r\n") == 0) data_end -= 2;

        MultipartPart p;
        const size_t nm = headers.find("name=\"");
        if (nm != std::string::npos) {
            const size_t e = headers.find('"', nm + 6);
            if (e != std::string::npos) p.name = headers.substr(nm + 6, e - (nm + 6));
        }
        const size_t fn = headers.find("filename=\"");
        if (fn != std::string::npos) {
            const size_t e = headers.find('"', fn + 10);
            if (e != std::string::npos) p.filename = headers.substr(fn + 10, e - (fn + 10));
        }
        p.content = body.substr(data_start, data_end - data_start);
        out.push_back(std::move(p));
        if (next == std::string::npos) break;
        cursor = data_end;
    }
    return out;
}

/// 取第一个 part 当文件（老的调用方：平台推模型、OTA 传固件，都不关心字段名）
bool extract_multipart_file(const std::string& body, const std::string& content_type,
                            std::string& filename, std::string& content) {
    const std::vector<MultipartPart> parts = parse_multipart(body, content_type);
    if (parts.empty()) return false;
    if (!parts[0].filename.empty()) filename = parts[0].filename;
    content = parts[0].content;
    return true;
}

}  // namespace routes
}  // namespace capp
