// Demo 卡片与动作脚本
//
// 入口：register_demo_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 对应 app/routes/demo.py + 动作脚本那条 /api/demo/run。

#include "routes_internal.hpp"

#include <cstdio>
#include <unistd.h>

namespace capp {
namespace routes {

// ── Demo 卡片与动作脚本 ──

void register_demo_routes(Router& router, AppContext& ctx) {


    // ── 动作脚本（demo/*.lua）── 接口都挂在 /api/demo 下（跟卡片/配置同一套命名）
    //
    // `/api/demo/run` 是**最底层**的那条：直接跑某个动作脚本 + 任意 params（不校验模型），
    // 调试/一次性用；正常跑 demo 走 `/api/demo/init`（跑卡片，或 action+model，会先校验
    // 动作脚本和模型文件都在）。
    // 把"看→对准→靠近→抓"这类要反复调参的流程写成脚本，改一行存盘重跑，不用重编部署。
    // 安全兜底（限速/被人的指令取代/底盘掉线/内存与卡死）全在宿主里，脚本绕不过去。
    router.add("POST", "/api/demo/run", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        const std::string name = payload.gets("script");
        if (name.empty()) {
            resp.set_error("script 必填（动作名，例：grab）", 400);
            return;
        }
        // params 原样给脚本（含 mode=once|loop）；**没有 max_seconds**，跑多久看模式与停止
        const Json* params = payload.get("params");
        // **默认就等它跑完**（调用方一个请求就能拿到"做完了没有"）；显式传 "wait": false 才立刻返回。
        // 但 loop 模式不会自己结束 —— 那种情况默认**不等**（否则等于把连接挂死），
        // 只有显式要求 wait 才 400（那是真没意义）。
        const bool has_wait = payload.get("wait") != nullptr;
        const bool loop_mode = params && params->gets("mode") == "loop";
        if (loop_mode && has_wait && payload.getb("wait", true)) {
            resp.set_error("loop 模式不会自己结束，wait 没有意义（要停就 POST /api/demo/stop）", 400);
            return;
        }
        const bool wait = loop_mode ? false : (has_wait ? payload.getb("wait", true) : true);
        Json r = script_run(ctx, name, params ? *params : Json());
        if (!r.getb("ok") || !wait) {
            if (r.getb("ok")) r["completed"] = false;   // 只是"起来了"，还没跑完
            resp.set_json(r, r.getb("ok") ? 200 : 400);
            return;
        }
        const bool done = wait_script_done(ctx, kDemoWaitMaxSeconds);
        const Json st = script_status(ctx);
        // 同样精简到一个标志（与 /api/control 的 completed 同一个含义）
        Json out;
        out["completed"] = done && st.gets("state") == "done";
        if (!out.getb("completed")) {
            out["error"] = done ? st.gets("message")
                                : "timeout: 等了 " + std::to_string((long long)kDemoWaitMaxSeconds) +
                                      " 秒还没跑完（脚本卡在不调原语的死循环里？试 POST /api/demo/stop）";
        }
        resp.set_json(out);
    });

    router.add("GET", "/api/demo/status", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(script_status(ctx));
    });

    // ── /api/demo ── 一张卡片 = **动作 × 模型**（用户在界面上新建，见文件上方 DemoCard 的说明）
    //
    // 前端契约：卡片名仍然是 name（前端拿它当 key 与显示），另给 action/model/action_name；
    // "新建卡片"要用的动作清单与模型清单也跟着 list 一起回，省一次请求。
    router.add("GET", "/api/demo/list", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        Json demos(Json::Type::Array);
        for (const auto& c : list_demo_cards(ctx)) {
            const bool has_action = action_script_exists(ctx, c.action);
            const bool has_model = access(model_path(ctx, c.model).c_str(), F_OK) == 0;
            Json item;
            item["name"] = c.name;                                  // 卡片名（前端只认这个）
            item["action"] = c.action;
            item["model"] = c.model;
            item["script"] = has_action ? c.action : "";            // 兼容老字段：动作名
            item["path"] = has_model ? model_path(ctx, c.model) : "";
            item["kind"] = "card";
            // 动作脚本或模型文件缺了也照样列出来 —— 点开始会明确报错，别让卡片凭空消失
            item["ready"] = has_action && has_model;
            item["error"] = !has_action ? ("动作脚本缺失：demo/" + c.action + ".lua")
                          : (!has_model ? ("模型文件缺失：demo/models/" + c.model + ".cvimodel") : "");
            demos.push_back(item);
        }
        Json actions(Json::Type::Array);
        for (const auto& a : list_actions(ctx)) {
            Json x;
            x["id"] = a.id;
            x["name"] = a.name;      // 脚本第一行 `-- name: 接近瞄准` 给的显示名
            actions.push_back(x);
        }
        Json models(Json::Type::Array);
        for (const auto& m : list_models(ctx)) models.push_back(m);
        Json j;
        j["demos"] = demos;
        j["actions"] = actions;
        j["models"] = models;
        resp.set_json(j);
    });

    router.add("GET", "/api/demo/name", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json st = script_status(ctx);
        Json j;
        j["name"] = st.gets("card");       // 跑的是哪张卡片
        j["action"] = st.gets("script");   // 动作脚本名
        j["model"] = st.gets("model");
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/init", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }

        // 两种调用方式：
        //   ① {"name":"追网球接近"}                       ← 界面点"开始"（跑存下来的那张卡片）
        //   ② {"action":"approach","model":"tennis",...}  ← 直接跑，不用建卡
        //      （"刚传上来一个新模型，立刻用它接近一下"就是这条）
        std::string action, model, card;
        Json params;
        const std::string name = payload.gets("name");
        if (!name.empty()) {
            if (!valid_card_name(name)) {
                resp.set_error("卡片名非法：" + name, 400);
                return;
            }
            DemoCard c;
            if (!load_demo_card(ctx, name, c)) {
                resp.set_error("没有这张卡片（或配置读不了）：demo/configs/" + name + ".json", 400);
                return;
            }
            action = c.action;
            model = c.model;
            card = name;
            params = c.params;
        } else {
            action = payload.gets("action");
            model = payload.gets("model");
            if (action.empty() || model.empty()) {
                resp.set_error("要么给 name（跑已建的卡片），要么给 action + model（直接跑）", 400);
                return;
            }
            params["target_size"] = Json((int64_t)payload.geti("target_size", kDemoTargetSizeDefault));
            params["speed"] = Json((int64_t)payload.geti("speed", kDemoSpeedDefault));
            params["turn_speed"] = Json((int64_t)payload.geti("turn_speed", kDemoTurnSpeedDefault));
            params["mode"] = (payload.gets("mode") == "loop") ? "loop" : "once";
        }

        // 名字都要拼进路径，且必须真存在 —— 在这里挡掉，别让它变成脚本里一句含糊的报错
        if (!valid_model_name(action)) {
            resp.set_error("动作名非法（只允许字母数字与 _ - .）：" + action, 400);
            return;
        }
        if (!valid_model_name(model)) {
            resp.set_error("模型名非法（只允许字母数字与 _ - .）：" + model, 400);
            return;
        }
        if (!action_script_exists(ctx, action)) {
            resp.set_error("动作脚本不存在：demo/" + action + ".lua", 400);
            return;
        }
        if (access(model_path(ctx, model).c_str(), F_OK) != 0) {
            resp.set_error("模型不存在：demo/models/" + model + ".cvimodel", 400);
            return;
        }

        // 请求里显式传的参数优先（卡片里那份作底）
        if (payload.get("target_size")) params["target_size"] = Json(payload.geti("target_size", kDemoTargetSizeDefault));
        if (payload.get("speed")) params["speed"] = Json(payload.geti("speed", kDemoSpeedDefault));
        if (payload.get("turn_speed")) params["turn_speed"] = Json(payload.geti("turn_speed", kDemoTurnSpeedDefault));
        if (payload.get("mode")) params["mode"] = payload.gets("mode");

        // **默认等它跑完**（一个请求拿到完成标志）；显式 "wait": false 才立刻回 started。
        // loop 模式不会自己结束：默认不等（否则挂死连接），只有显式要求才 400。
        const bool has_wait = payload.get("wait") != nullptr;
        const bool loop_mode = params.gets("mode") == "loop";
        if (loop_mode && has_wait && payload.getb("wait", true)) {
            resp.set_error("loop 模式不会自己结束，wait 没有意义（要停就 POST /api/demo/stop）", 400);
            return;
        }
        const bool wait = loop_mode ? false : (has_wait ? payload.getb("wait", true) : true);

        // ★ 模型来自卡片/请求，**不是卡片名** —— 搞错的话脚本会去开
        //   demo/models/<卡片名>.cvimodel，报错长成"注册模型失败"，极具误导性
        params["model"] = model;
        params["card"] = card;   // 让状态能回答"现在跑的是哪张卡"；脚本不用管它

        const Json r = script_run(ctx, action, params);

        if (!r.getb("ok")) {
            const Json st = script_status(ctx);
            Json j;
            j["status"] = "already_running";
            j["pid"] = Json((int64_t)getpid());
            j["name"] = st.gets("card");
            j["error"] = r.gets("error");
            resp.set_json(j, 409);
            return;
        }
        Json j;
        j["status"] = "started";
        j["name"] = card.empty() ? action : card;   // 卡片名（没建卡直接跑时回动作名）
        j["script"] = action;
        j["action"] = action;
        j["model"] = model;
        j["pid"] = Json((int64_t)getpid());   // 兼容字段：跑 demo 的进程就是 capp 自己
        j["pgid"] = Json((int64_t)getpid());
        j["completed"] = false;               // 只是"起来了"，还没跑完
        if (!wait) {
            resp.set_json(j);                 // 默认：立刻回 started，界面靠 /api/demo/status 轮询
            return;
        }
        // 跑完再返回：字段与 /api/demo/status 一致 + status（finished / timeout）+ completed
        const bool done = wait_script_done(ctx, kDemoWaitMaxSeconds);
        const Json st = script_status(ctx);
        Json out;
        out["completed"] = done && st.gets("state") == "done";
        if (!out.getb("completed")) {
            out["error"] = done ? st.gets("message")
                                : "timeout: 等了 " + std::to_string((long long)kDemoWaitMaxSeconds) +
                                      " 秒还没跑完（脚本卡在不调原语的死循环里？试 POST /api/demo/stop）";
        }
        resp.set_json(out);
    });

    // 卡片配置：GET 读一张、POST 新建或覆盖（动作 + 模型 + 四个参数）
    router.add("GET", "/api/demo/config", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const std::string name = req.query_param("name");
        if (name.empty()) {
            resp.set_error("name 必填（卡片名，如 ?name=追网球接近）", 400);
            return;
        }
        if (!valid_card_name(name)) {
            resp.set_error("卡片名非法（不能含 / \\ 与控制字符，不能以 . 开头）：" + name, 400);
            return;
        }
        DemoCard c;
        if (!load_demo_card(ctx, name, c)) {
            resp.set_error("没有这张卡片（或配置读不了）：demo/configs/" + name + ".json", 400);
            return;
        }
        Json j = c.params;
        j["name"] = c.name;
        j["action"] = c.action;
        j["model"] = c.model;
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/config", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        if (!payload.is_object()) {
            resp.set_error("json body is required", 400);
            return;
        }
        const std::string name = payload.gets("name");
        if (name.empty()) {
            resp.set_error("name 必填（卡片名，如 追网球接近）", 400);
            return;
        }
        if (!valid_card_name(name)) {
            resp.set_error("卡片名非法（不能含 / \\ 与控制字符，不能以 . 开头）：" + name, 400);
            return;
        }
        const std::string action = payload.gets("action");
        const std::string model = payload.gets("model");
        if (action.empty() || model.empty()) {
            resp.set_error("action 与 model 必填（这张卡片跑哪个动作、用哪个模型）", 400);
            return;
        }
        if (!valid_model_name(action)) {
            resp.set_error("动作名非法（只允许字母数字与 _ - .）：" + action, 400);
            return;
        }
        if (!valid_model_name(model)) {
            resp.set_error("模型名非法（只允许字母数字与 _ - .）：" + model, 400);
            return;
        }
        if (!action_script_exists(ctx, action)) {
            resp.set_error("动作脚本不存在：demo/" + action + ".lua", 400);
            return;
        }
        if (access(model_path(ctx, model).c_str(), F_OK) != 0) {
            resp.set_error("模型不存在：demo/models/" + model + ".cvimodel", 400);
            return;
        }
        if (!save_demo_card(ctx, name, action, model, payload)) {
            resp.set_error("写入 demo/configs/" + name + ".json 失败", 500);
            return;
        }
        Json j;
        j["ok"] = true;
        j["name"] = name;
        j["action"] = action;
        j["model"] = model;
        j["target_size"] = Json((int64_t)payload.geti("target_size", kDemoTargetSizeDefault));
        j["speed"] = Json((int64_t)payload.geti("speed", kDemoSpeedDefault));
        j["turn_speed"] = Json((int64_t)payload.geti("turn_speed", kDemoTurnSpeedDefault));
        j["mode"] = (payload.gets("mode") == "loop") ? "loop" : "once";
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/delete", [&ctx](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json payload = req.json();
        const std::string name = payload.is_object() ? payload.gets("name") : "";
        if (name.empty()) {
            resp.set_error("name 必填（要删的卡片名）", 400);
            return;
        }
        if (!valid_card_name(name)) {
            resp.set_error("卡片名非法：" + name, 400);
            return;
        }
        const std::string path = demo_config_path(ctx, name);
        if (std::remove(path.c_str()) != 0) {
            resp.set_error("没有这张卡片（或删不掉）：demo/configs/" + name + ".json", 400);
            return;
        }
        Json j;
        j["ok"] = true;
        j["name"] = name;
        resp.set_json(j);
    });

    router.add("POST", "/api/demo/stop", [&ctx](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        const Json r = script_stop(ctx);
        const Json st = script_status(ctx);
        Json j;
        j["status"] = r.gets("state") == "idle" ? "already_stopped" : "stopped";
        j["name"] = st.gets("card");
        j["action"] = st.gets("script");
        resp.set_json(j);
    });
}
}  // namespace routes
}  // namespace capp
