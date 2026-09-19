// 单帧推理 + 模型文件
//
// 入口：register_models_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// C++ 比 Python 原版多出来的域（/api/detect、/api/models/*）。

#include "routes_internal.hpp"

#include "csrc/log.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace capp {
namespace routes {

/// 去掉首尾空白（multipart 文本字段带不带换行看客户端，不能想当然）
std::string trim_ws(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// 文件名是不是 .cvimodel（大小写不敏感）—— 训练平台固定发 model.cvimodel，
/// 后缀不对说明发错文件了，早点挡掉比在模型注册时报错清楚。
bool has_cvimodel_ext(const std::string& filename) {
    if (filename.size() < 9) return false;
    std::string tail = filename.substr(filename.size() - 9);
    for (char& c : tail) c = (char)tolower((unsigned char)c);
    return tail == ".cvimodel";
}

// ── 单帧推理 + 模型文件 ──

void register_models_routes(Router& router, AppContext& ctx) {


    // 单帧推理：取当前帧跑一次模型，只回框的四个角（原图像素坐标）。
    // 模型必填（裸名字 → $AKA_HOME/demo/models/<名字>.cvimodel）；可选阈值 ?conf=&iou=
    // （不给用默认 0.25 / 0.45，给错值直接 400）。
    router.add("GET", "/api/detect", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const std::string model = req.query_param("model");
        if (model.empty()) {
            Json j;
            j["ok"] = false;
            j["error"] = "缺少 model 参数（例：/api/detect?model=tennis）";
            resp.set_json(j, 400);
            return;
        }
        if (!valid_model_name(model)) {
            Json j;
            j["ok"] = false;
            j["error"] = "model 名字非法（只允许字母数字与 _ - .）：" + model;
            resp.set_json(j, 400);
            return;
        }
        // 可选阈值：?conf=0.6&iou=0.3（不给就用默认 0.25 / 0.45 = 与以前完全一样）。
        // 给错值直接报错而不是悄悄用默认 —— 调参时最怕"以为生效了其实没生效"。
        double conf = 0, iou = 0;   // 0 = 没给，交给 decode_options 取默认
        auto read_thresh = [&](const char* key, double& out) -> bool {
            const std::string v = req.query_param(key);
            if (v.empty()) return true;
            char* end = nullptr;
            const double d = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || d <= 0 || d >= 1) return false;
            out = d;
            return true;
        };
        if (!read_thresh("conf", conf) || !read_thresh("iou", iou)) {
            Json j;
            j["ok"] = false;
            j["error"] = "conf / iou 要在 0~1 之间（如 ?conf=0.6&iou=0.3）；不给就用默认 0.25 / 0.45";
            resp.set_json(j, 400);
            return;
        }
        const Json j = detect_once(ctx, model, decode_options(conf, iou));
        resp.set_json(j, j.getb("ok") ? 200 : 500);
    });

    // 模型上传：**平台把模型文件直接推给小车**（小车在内网，未必能反过来访问平台）。
    // 名字走 query（?name=tennis），文件放请求体：
    //   raw：     curl --data-binary @tennis.cvimodel "http://<ip>/api/models/upload?name=tennis"
    //   multipart：curl -F "file=@tennis.cvimodel"  "http://<ip>/api/models/upload?name=tennis"
    // 同步返回（3.5MB 的体很小，写完即回），同名覆盖、覆盖即生效。
    router.add("POST", "/api/models/upload", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const std::string name = req.query_param("name");
        if (name.empty()) {
            resp.set_error("name 参数必填（例：?name=tennis）", 400);
            return;
        }
        if (!valid_model_name(name)) {
            resp.set_error("name 非法（只允许字母数字与 _ - .）：" + name, 400);
            return;
        }
        std::string content = req.body;
        const std::string ct = req.header("content-type");
        if (ct.find("multipart/form-data") != std::string::npos) {
            std::string filename;   // 名字以 ?name= 为准，这里只取文件内容
            if (!extract_multipart_file(req.body, ct, filename, content)) {
                resp.set_error("multipart 解析失败（缺 file 字段？）", 400);
                return;
            }
        }
        // 超过服务器上限的体不会被读进来（req.body 是空的），单独给个明确的原因，
        // 否则调用方只会看到含糊的"请求体为空"。
        const std::string cl_hdr = req.header("content-length");
        if (!cl_hdr.empty() && atoll(cl_hdr.c_str()) > kMaxRequestBody) {
            resp.set_error("文件过大：" + cl_hdr + " 字节，上限 " +
                               std::to_string(kMaxRequestBody / (1024 * 1024)) + "MB",
                           413);
            return;
        }
        if (content.empty()) {
            resp.set_error("请求体为空（把模型文件放进 body）", 400);
            return;
        }
        const Json r = save_model_upload(ctx, name, content);
        resp.set_json(r, r.getb("ok") ? 200 : 400);
    });

    // 模型删除：**只删 demo/models/<名字>.cvimodel 这一个文件**。
    // 卡片配置不动 —— 用到它的卡片照样列在 Demo 页上，只是 ready=false、点开始会明确报
    // "模型文件缺失"（不清卡片是故意的：重传一个同名模型就原地复活）。
    // 响应里回一份"哪些卡片在用它"，界面删之前就能把后果说清楚。
    router.add("POST", "/api/models/delete", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        const std::string name = payload.is_object() ? payload.gets("name") : "";
        if (name.empty()) {
            resp.set_error("name 必填（要删的模型名，不带 .cvimodel）", 400);
            return;
        }
        // 和上传同一个校验：名字会拼进路径，禁 / 与 ..
        if (!valid_model_name(name)) {
            resp.set_error("模型名非法（只允许字母数字与 _ - .）：" + name, 400);
            return;
        }
        const std::string path = model_path(ctx, name);
        if (access(path.c_str(), F_OK) != 0) {
            resp.set_error("没有这个模型：demo/models/" + name + ".cvimodel", 400);
            return;
        }
        if (unlink(path.c_str()) != 0) {
            resp.set_error("删除失败（" + std::string(std::strerror(errno)) + "）：" + path, 500);
            return;
        }
        CAM_INFO("[models] 模型已删除 %s", path.c_str());
        Json j;
        j["ok"] = true;
        j["name"] = name;
        Json used(Json::Type::Array);
        for (const auto& c : list_demo_cards(ctx)) {
            if (c.model == name) used.push_back(c.name);
        }
        j["cards"] = used;   // 用着它的卡片（界面拿来提示"这些会变成模型缺失"）
        resp.set_json(j);
    });

    // ── 训练平台直传模型（浏览器 → 小车，同一局域网；yolotrain.chenlongrobot.com）──
    //
    // 与上面 `/api/models/upload` 的区别：那个是"平台/curl 推模型"（名字走 query，body 就是
    // 文件裸内容，响应 {ok,name,path,size}）；这个是**浏览器表单直传**（multipart 两个字段
    // file+name，响应 {status,name,size}）。
    // **不再给模型生成脚本**：动作脚本是预定义、与模型无关的，传完模型后在 Demo 页建一张卡
    // （动作 × 这个模型），或直接 POST /api/demo/init {"action":"grab","model":"<名字>"}。
    //
    // CORS 与 OPTIONS 预检不在这里处理：http_server 在路由之前就统一应答了（所有响应也
    // 自动带 Access-Control-Allow-Origin: *），浏览器跨域直传本来就要求那样。
    router.add("POST", "/api/model/upload", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        auto fail = [&resp](const std::string& msg) {
            Json e;
            e["status"] = "error";
            e["message"] = msg;
            resp.set_json(e, 400);
        };

        const std::string ct = req.header("content-type");
        std::string filename, content, name;
        if (ct.find("multipart/form-data") != std::string::npos) {
            for (const auto& p : parse_multipart(req.body, ct)) {
                // 字段名以表单为准；两个字段谁先到不确定，所以是遍历而不是取第一个 part
                if (p.name == "file") {
                    filename = p.filename;
                    content = p.content;
                } else if (p.name == "name") {
                    name = trim_ws(p.content);
                }
            }
        }
        if (name.empty()) name = trim_ws(req.query_param("name"));   // ?name= 兜底

        // 逐条按契约校验，失败一律 400 + {status:"error", message}
        if (content.empty()) {
            fail("invalid file");
            return;
        }
        if (!has_cvimodel_ext(filename)) {   // 后缀不对 = 发错文件了（平台固定发 model.cvimodel）
            fail("invalid file");
            return;
        }
        if (name.empty()) {
            fail("invalid name");
            return;
        }
        if (!valid_model_name(name)) {   // 名字要拼进路径：`../../etc/passwd` 挡在这里
            fail("invalid name");
            return;
        }

        const Json r = save_model_upload(ctx, name, content);
        if (!r.getb("ok")) {
            fail(r.gets("error"));   // 魔数不对 / 过大 / 换入失败 —— 原因比"invalid file"有用
            return;
        }
        Json j;
        j["status"] = "ok";
        j["name"] = name;
        j["size"] = Json((int64_t)r.geti("size", 0));
        j["path"] = r.gets("path");
        // script / script_created：训练平台那份契约里的字段，**保留不删**（平台在读），
        // 但语义变了 —— 动作脚本是预定义的、与模型无关，上传模型不再生成脚本。
        // 模型传上来就能用：建一张卡片（动作 × 这个模型）或直接
        // POST /api/demo/init {"action":"grab","model":"<名字>"}。
        j["script"] = "";
        j["script_created"] = false;
        // 顺手把可用的动作清单带上，平台侧想提示"能用哪些动作"就有数据了
        Json actions(Json::Type::Array);
        for (const auto& a : list_actions(ctx)) actions.push_back(a.id);
        j["actions"] = actions;
        resp.set_json(j);
    });
}
}  // namespace routes
}  // namespace capp
