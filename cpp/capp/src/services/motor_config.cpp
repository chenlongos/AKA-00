// 底盘左右接线标定（网页「左右方向切换」）的落盘与读取
//
// 文件：$AKA_HOME/motor_config.json   {"swap_lr": false}
//
// 为什么单独一个文件、而不是只改 config.toml：C++ 侧的标定本来在 config.toml 的
// [motor] swap_lr 里，但 Python 路径（run.py）根本不读 config.toml（它的配置是
// app/config.py 里的常量）。网页上这一个开关要"两条路径都能用"，所以落成两边都能
// 读写的小 JSON，放在同一个 app_dir（板上就是 $AKA_HOME）下 —— Python 侧见
// app/motor_config.py，同一个文件名、同一个位置。
//
// 优先级：motor_config.json > config.toml [motor] swap_lr —— 网页点过开关（文件存在）
// 就以网页为准；没有这个文件才沿用 config.toml（现场手改/老固件带过来的值）。

#include "capp/context.hpp"

#include "csrc/json.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace capp {

namespace {

std::string motor_config_path(const AppContext& ctx) {
    return ctx.app_dir + "/motor_config.json";
}

}  // namespace

bool load_motor_swap_lr(const AppContext& ctx, bool fallback) {
    std::ifstream f(motor_config_path(ctx));
    if (!f) return fallback;
    std::stringstream ss;
    ss << f.rdbuf();
    csrc::Json data;
    if (!csrc::Json::parse(ss.str(), data) || !data.is_object()) return fallback;
    const csrc::Json* p = data.get("swap_lr");
    // 类型不对就当没配（别把 "true" 字符串之类猜成布尔）
    if (p == nullptr || !p->is_bool()) return fallback;
    return p->as_int(0) != 0;   // Json 没有 as_bool，is_bool 的 as_int 就是 1/0
}

bool save_motor_swap_lr(const AppContext& ctx, bool swap_lr) {
    csrc::Json j;
    j["swap_lr"] = csrc::Json(swap_lr);
    const std::string path = motor_config_path(ctx);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump(false) << "\n";
        if (!f.good()) return false;
    }
    // .tmp + rename：断电不会留下半份文件
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace capp