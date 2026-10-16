#include "routes_internal.hpp"
#include "capp/program_service.hpp"

namespace capp { namespace routes {
void register_program_routes(Router& router, AppContext& ctx) {
    if (!ctx.program_service) ctx.program_service = std::make_shared<ProgramService>(ctx);
    auto service = ctx.program_service;
    router.add("GET", "/api/program/capabilities", [service](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(service->capabilities());
    });
    router.add("POST", "/api/program/upload", [service](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        if (req.body.size() > 512 * 1024) { resp.set_error("程序包超过 512 KiB", 413); return; }
        int status = 200; auto result = service->upload(req.json(), status); resp.set_json(result, status);
    });
    router.add("POST", "/api/program/run", [service](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        int status = 202; auto result = service->run(req.json().gets("programId"), status); resp.set_json(result, status);
    });
    router.add("POST", "/api/program/deploy", [service](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        if (req.body.size() > 512 * 1024) { resp.set_error("程序包超过 512 KiB", 413); return; }
        int status = 202; auto result = service->deploy(req.json(), status); resp.set_json(result, status);
    });
    router.add("POST", "/api/program/stop", [service](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        int status = 200; auto result = service->stop(req.json().gets("runId"), status); resp.set_json(result, status);
    });
    router.add("GET", "/api/program/status", [service](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        resp.set_json(service->status());
    });
    router.add("GET", "/api/program/logs", [service](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        char* end = nullptr; const std::string value = req.query_param("after", "0");
        long long after = strtoll(value.c_str(), &end, 10);
        if (!end || *end || after < 0) { resp.set_error("日志序号无效", 400); return; }
        resp.set_json(service->logs(req.query_param("runId"), after));
    });
}
} }  // namespace capp::routes
