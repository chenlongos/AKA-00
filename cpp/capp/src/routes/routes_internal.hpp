// capp/src/routes/routes_internal.hpp —— 路由层内部共享的东西
//
// 这一层是 `app/routes/*.py` 的 C++ 对应物：一个域一个文件，各自的注册函数由
// `src/routes.cpp` 的 `register_routes()` 依次调用（对外入口没变，main.cpp 不用动）。
//
// 放在这里的只有**跨文件共用**的东西：各域的注册函数、demo 卡片（demo.cpp 与 models.cpp
// 都要读）、multipart 解析（models.cpp 与 ota.cpp 都要用）。
// 只在一个文件里用的辅助（semver、速度配置、trim_ws、wpa 自举…）就跟它的域待在一起，
// 别往这里堆。

#pragma once

#include <string>
#include <vector>

#include "capp/context.hpp"
#include "capp/http_server.hpp"

namespace capp {
namespace routes {

using Json = csrc::Json;

// ── 各域的路由注册（实现见同目录同名 .cpp；顺序即注册顺序，互不覆盖）──
void register_motor_routes(Router& router, AppContext& ctx);
void register_arm_routes(Router& router, AppContext& ctx);
void register_camera_routes(Router& router, AppContext& ctx);
void register_models_routes(Router& router, AppContext& ctx);
void register_demo_routes(Router& router, AppContext& ctx);
void register_display_routes(Router& router, AppContext& ctx);
void register_ota_routes(Router& router, AppContext& ctx);
void register_system_routes(Router& router, AppContext& ctx);
void register_wifi_routes(Router& router, AppContext& ctx);
void register_config_routes(Router& router, AppContext& ctx);
void register_ws_routes(Router& router, AppContext& ctx);

// ── demo 卡片（**动作 × 模型**），实现在 demo_card.cpp ──
//
// 一张卡片 = 一份 JSON：$AKA_HOME/demo/configs/<卡片名>.json
//     {"action":"approach","model":"tennis","target_size":300,"speed":30,...}
// 卡片由**用户在界面上新建**（选动作 + 选模型 + 填参数），接口是 POST /api/demo/config。
// 卡片名**只当文件名用**（可以是中文「追网球接近」），动作和模型写在文件里 ——
// 所以不需要"从名字拆出模型和动作"（那种拆法遇到模型名自带 `-` 就歧义了）。
// 跑卡片时：读配置 → 跑 demo/<动作>.lua → 把 params.model 注入成**配置里的模型**。
// （注意不是卡片名！搞错的话会变成"注册模型失败：…/demo/models/追网球接近.cvimodel"）

constexpr int kDemoTargetSizeDefault = 300;
constexpr int kDemoSpeedDefault = 25;        // 直线速度（%）
constexpr int kDemoTurnSpeedDefault = 25;    // 转弯速度（%）—— 和直线分开：转弯要的占空比不同
// 执行方式（卡片上一个字段）：跑一遍就结束 / 跑完接着跑直到被停
// （目前没人读这个常量，比大小写都在各自的地方硬编码 —— 留着当文档，别删）
constexpr const char* kDemoModeDefault = "once";
// "等它跑完再返回"（默认就等）最多等多久 —— 超时就回 timeout + 当前状态，不无限挂着。
// 这个数要**比脚本自己的"执行一次"时限（script.cpp 的 kScriptMaxOnceSeconds）略大**：
// 那样到点的正常路径是脚本先收工、请求拿到真实原因（"到最大执行时间"），而不是请求先不等了、
// 留一辆还在动的车。多出来的 10 秒就是给这条留的余量。
// 循环执行不受它影响：loop 默认就不等（等了也不会自己结束）。
constexpr double kDemoWaitMaxSeconds = 310.0;

struct DemoCard {
    std::string name;     // 卡片名（= 文件名，可能中文）
    std::string action;   // 动作 = 脚本名（demo/<action>.lua）
    std::string model;    // 模型（demo/models/<model>.cvimodel）
    csrc::Json params;    // 四个运行参数（缺的用默认值兜底）
};

std::string demo_config_path(AppContext& ctx, const std::string& name);

/// 读一张卡片；不存在 / 解析失败 / 名字非法 / 没写 action 或 model → 返回 false
bool load_demo_card(AppContext& ctx, const std::string& name, DemoCard& out);

/// 列全部卡片（按名字排序）；读不了的那张跳过
std::vector<DemoCard> list_demo_cards(AppContext& ctx);

/// 写一张卡片（新建或覆盖）
bool save_demo_card(AppContext& ctx, const std::string& name, const std::string& action,
                    const std::string& model, const csrc::Json& params);

/// 可用的动作脚本（demo/*.lua）；name 来自脚本第一行的 `-- name: 显示名`
struct ActionInfo {
    std::string id;
    std::string name;
};
std::vector<ActionInfo> list_actions(AppContext& ctx);

/// 可用的模型（demo/models/*.cvimodel，去掉后缀）
std::vector<std::string> list_models(AppContext& ctx);

// ── multipart/form-data（实现在 multipart.cpp）──
//
// 一个 part 的三样东西：字段名 name、文件名 filename（文件字段才有）、内容 content。
// 注意**不能只取第一个 part**：训练平台的表单是 file + name 两个字段，先来哪个不确定。
struct MultipartPart {
    std::string name;
    std::string filename;
    std::string content;
};

std::vector<MultipartPart> parse_multipart(const std::string& body,
                                           const std::string& content_type);

/// 从 multipart 体里取第一个文件字段的内容；没有文件字段返回 false
bool extract_multipart_file(const std::string& body, const std::string& content_type,
                            std::string& filename, std::string& content);

}  // namespace routes
}  // namespace capp
