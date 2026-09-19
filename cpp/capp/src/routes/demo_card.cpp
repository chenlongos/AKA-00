// Demo 卡片：一份 configs/<卡片名>.json 的读写
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 供 demo.cpp（跑卡片）与 models.cpp（删模型时报“哪些卡片在用”）共用。

#include "routes_internal.hpp"

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <sstream>

#include "csrc/system_utils.hpp"   // csrc::ensure_dir（建 demo/configs/）

namespace capp {
namespace routes {

// 卡片的结构、默认值与"为什么卡片名只当文件名"那段说明都在 routes_internal.hpp。

/// 卡片配置目录（只有这个文件用，不外传）
std::string demo_config_dir(AppContext& ctx) { return ctx.app_dir + "/demo/configs"; }

std::string demo_config_path(AppContext& ctx, const std::string& name) {
    return demo_config_dir(ctx) + "/" + name + ".json";
}

/// 读一张卡片；不存在 / 解析失败 / 名字非法 / 没写 action 或 model → 返回 false
bool load_demo_card(AppContext& ctx, const std::string& name, DemoCard& out) {
    if (!valid_card_name(name)) return false;
    std::ifstream f(demo_config_path(ctx, name));
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    csrc::Json one;
    if (!csrc::Json::parse(ss.str(), one) || !one.is_object()) return false;

    out.name = name;
    out.action = one.gets("action");
    out.model = one.gets("model");
    out.params = csrc::Json();
    out.params["target_size"] = csrc::Json((int64_t)one.geti("target_size", kDemoTargetSizeDefault));
    out.params["speed"] = csrc::Json((int64_t)one.geti("speed", kDemoSpeedDefault));
    out.params["turn_speed"] = csrc::Json((int64_t)one.geti("turn_speed", kDemoTurnSpeedDefault));
    const std::string mode = one.gets("mode");
    out.params["mode"] = (mode == "loop") ? "loop" : "once";
    return !out.action.empty() && !out.model.empty();
}

/// 列出所有卡片：扫 demo/configs/*.json（= 卡片就是配置，没有单独的注册表）
std::vector<DemoCard> list_demo_cards(AppContext& ctx) {
    std::vector<DemoCard> out;
    const std::string dir = demo_config_dir(ctx);
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() <= 5 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
        DemoCard c;
        if (load_demo_card(ctx, n.substr(0, n.size() - 5), c)) out.push_back(c);
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const DemoCard& x, const DemoCard& y) { return x.name < y.name; });
    return out;
}

/// 写一张卡片（新建或覆盖）
bool save_demo_card(AppContext& ctx, const std::string& name, const std::string& action,
                    const std::string& model, const csrc::Json& params) {
    if (!valid_card_name(name)) return false;
    // demo/configs/ 可能还不存在（板上第一次建卡时），而 ofstream 不会建目录
    if (!csrc::ensure_dir(demo_config_dir(ctx))) return false;
    csrc::Json one;
    one["action"] = action;
    one["model"] = model;
    one["target_size"] = csrc::Json((int64_t)params.geti("target_size", kDemoTargetSizeDefault));
    one["speed"] = csrc::Json((int64_t)params.geti("speed", kDemoSpeedDefault));
    one["turn_speed"] = csrc::Json((int64_t)params.geti("turn_speed", kDemoTurnSpeedDefault));
    one["mode"] = (params.gets("mode") == "loop") ? "loop" : "once";
    std::ofstream f(demo_config_path(ctx, name));
    if (!f) return false;
    f << one.dump(false);
    f.close();
    return (bool)f;
}

/// 动作清单：扫 demo/<动作>.lua（`_` 开头的跳过 —— 那是模板/草稿，不是一个动作）。
/// 显示名取脚本第一行的约定注释 `-- name: 接近瞄准`；没有就用文件名。
std::vector<ActionInfo> list_actions(AppContext& ctx) {
    std::vector<ActionInfo> out;
    const std::string dir = ctx.app_dir + "/demo";
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() <= 4 || n.compare(n.size() - 4, 4, ".lua") != 0) continue;
        if (n[0] == '_') continue;                        // 下划线开头的是模板/草稿，不算一个动作
        const std::string id = n.substr(0, n.size() - 4);
        if (!valid_model_name(id)) continue;              // 动作名要能拼进路径
        ActionInfo a;
        a.id = id;
        a.name = id;
        std::ifstream f(dir + "/" + n);
        std::string first;
        if (f && std::getline(f, first)) {
            const std::string key = "name:";
            const size_t at = first.find(key);
            if (at != std::string::npos) {
                std::string label = first.substr(at + key.size());
                const size_t b = label.find_first_not_of(" \t");
                const size_t e2 = label.find_last_not_of(" \t\r");
                if (b != std::string::npos) a.name = label.substr(b, e2 - b + 1);
            }
        }
        out.push_back(a);
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const ActionInfo& x, const ActionInfo& y) { return x.id < y.id; });
    return out;
}

/// 模型清单：扫 demo/models/*.cvimodel（"新建卡片"的下拉要用）
std::vector<std::string> list_models(AppContext& ctx) {
    std::vector<std::string> out;
    const std::string dir = model_dir(ctx);
    const std::string suffix = ".cvimodel";
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() <= suffix.size() ||
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        out.push_back(n.substr(0, n.size() - suffix.size()));
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace routes
}  // namespace capp
