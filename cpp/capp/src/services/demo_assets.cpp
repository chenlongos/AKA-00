// demo 资源：模型与动作脚本
//
// 由 capp/src/services.cpp 按域拆出来（对应 app/services/*.py 的分法）。
// 路径、命名校验、模型落盘（原子换入）。
// 声明都在 capp/context.hpp（那一份是按域分节的伞头文件，调用方只 include 它）。

#include "capp/context.hpp"

#include "capp/http_server.hpp"   // kMaxRequestBody（模型大小上限，与服务器同一个值）

#include "csrc/log.hpp"
#include "csrc/system_utils.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace capp {

namespace {

/// 校验临时文件（CviModel 魔数 + 大小上限）后原子换入最终路径；失败时删掉临时文件并填 err。
bool install_model_file(const std::string& tmp_path, const std::string& final_path,
                        long long max_bytes, long long& size_out, std::string& err) {
    std::ifstream f(tmp_path, std::ios::binary);
    if (!f) {
        err = "临时文件打不开";
        return false;
    }
    char magic[8] = {0};
    f.read(magic, sizeof magic);
    f.seekg(0, std::ios::end);
    const long long sz = (long long)f.tellg();
    f.close();
    if (sz < (long long)sizeof magic || std::string(magic, sizeof magic) != "CviModel") {
        err = "不是 cvimodel（文件头不是 CviModel）";
        std::remove(tmp_path.c_str());
        return false;
    }
    if (sz > max_bytes) {
        err = "模型过大：" + std::to_string(sz / (1024 * 1024)) + "MB，上限 " +
              std::to_string(max_bytes / (1024 * 1024)) + "MB";
        std::remove(tmp_path.c_str());
        return false;
    }
    if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        err = std::string("换入失败：") + std::strerror(errno);
        std::remove(tmp_path.c_str());
        return false;
    }
    size_out = sz;
    return true;
}

}  // namespace

std::string model_dir(AppContext& ctx) { return ctx.app_dir + "/demo/models"; }

std::string model_path(AppContext& ctx, const std::string& name) {
    return model_dir(ctx) + "/" + name + ".cvimodel";
}

std::string action_script_path(AppContext& ctx, const std::string& name) {
    return ctx.app_dir + "/demo/" + name + ".lua";
}

bool action_script_exists(AppContext& ctx, const std::string& name) {
    return access(action_script_path(ctx, name).c_str(), F_OK) == 0;
}

csrc::Json save_model_upload(AppContext& ctx, const std::string& name, const std::string& content) {
    csrc::Json j;
    const std::string final_path = model_path(ctx, name);
    const std::string tmp_path = final_path + ".part";   // 先落 .part 再原子换入
    // 目录得自己建，而且**要递归**：`demo/` 一级在 OTA 之后一定在（包里带着），
    // 但裸 mkdir() 只建一层、返回值还容易被忽略，最后表现成"临时文件写不开"，白查。
    if (!csrc::ensure_dir(model_dir(ctx))) {
        j["ok"] = false;
        j["error"] = "建模型目录失败：" + model_dir(ctx);
        return j;
    }

    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            j["ok"] = false;
            j["error"] = "临时文件写不开：" + tmp_path;
            return j;
        }
        f.write(content.data(), (std::streamsize)content.size());
        f.close();
        if (!f) {
            std::remove(tmp_path.c_str());
            j["ok"] = false;
            j["error"] = "写临时文件失败（磁盘满？）";
            return j;
        }
    }

    long long sz = 0;
    std::string err;
    if (!install_model_file(tmp_path, final_path, capp::kMaxRequestBody, sz, err)) {
        j["ok"] = false;
        j["error"] = err;
        return j;
    }
    CAM_INFO("[models] 模型已上传 %s（%lld KB）", final_path.c_str(), sz / 1024);
    j["ok"] = true;
    j["name"] = name;
    j["path"] = final_path;
    j["size"] = csrc::Json((int64_t)sz);
    return j;
}

bool valid_model_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (name == "." || name == "..") return false;
    for (char ch : name) {
        const char c = (char)ch;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool valid_card_name(const std::string& name) {
    // 卡片名只当文件名用（demo/configs/<卡片名>.json），比 valid_model_name 宽松：
    // 用户要能用「追网球接近」这种中文名。仍然要挡住路径穿越与怪字符。
    if (name.empty() || name.size() > 64) return false;
    if (name.front() == '.') return false;                          // 别造隐藏文件
    if (name.find("..") != std::string::npos) return false;         // 路径穿越
    for (unsigned char c : name) {
        if (c == '/' || c == '\\') return false;
        if (c < 0x20 || c == 0x7f) return false;                    // 控制字符
    }
    return true;
}

/// 取一帧跑一次推理 → 框列表（原图像素坐标）。`/api/detect` 与脚本原语共用同一条链：
/// 模型懒加载 / 文件变了重载 / 取原生帧 / 推理。错误串与对外契约保持一致。

}  // namespace capp
