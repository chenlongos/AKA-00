// WiFi 扫描与连接
//
// 入口：register_wifi_routes()（由 src/routes.cpp 的 register_routes 调用）
//
// 由 capp/src/routes.cpp 按域拆出来（对照 app/routes/*.py 的分法）。
// 对应 app/routes/wifi.py（含 wpa_supplicant 自举）。

#include "routes_internal.hpp"

#include "csrc/base64.hpp"
#include "csrc/system_utils.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace capp {
namespace routes {

// wpa_supplicant 自举（移植自 app/routes/wifi.py 的 ensure_wpa_env）
// 若 wlan1 的控制接口未就绪，则拉起网卡并后台启动 wpa_supplicant。
// 与 Python 版的区别：不执行 killall，避免误杀 wlan0 上服务当前连接的 wpa_supplicant。
bool ensure_wpa_env() {
    const std::string ctrl  = "/var/run/wpa_supplicant";
    const std::string iface = "wlan1";
    const std::string sock  = ctrl + "/" + iface;

    struct stat st{};
    if (stat(sock.c_str(), &st) == 0) return true;        // 已就绪
    if (stat(ctrl.c_str(), &st) != 0) mkdir(ctrl.c_str(), 0700);

    csrc::exec_output("ip link set " + iface + " down 2>/dev/null");
    csrc::exec_output("ip link set " + iface + " up 2>/dev/null");
    usleep(500000);
    csrc::exec_output("wpa_supplicant -D nl80211 -i " + iface + " -C " + ctrl +
                      " -B >/dev/null 2>&1");
    for (int i = 0; i < 10; i++) {                        // 最多等 5s
        if (stat(sock.c_str(), &st) == 0) return true;
        usleep(500000);
    }
    return false;
}

// ── WiFi 扫描与连接 ──

void register_wifi_routes(Router& router, AppContext& ctx) {
    (void)ctx;   // 这一域的路由用不到 ctx —— 签名保持一致，调用方一视同仁


    // ── /api/wifi ──
    router.add("GET", "/api/wifi/ip", [](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        ensure_wpa_env();
        std::string status = csrc::exec_output(
            "wpa_cli -p /var/run/wpa_supplicant -i wlan1 status 2>/dev/null");
        bool connected = status.find("wpa_state=COMPLETED") != std::string::npos;
        std::string ip = connected ? csrc::iface_ip("wlan1") : "192.168.4.1";
        if (ip.empty()) ip = "192.168.4.1";
        Json j;
        j["ip"] = ip;
        resp.set_json(j);
    });

    router.add("GET", "/api/wifi/status", [](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        ensure_wpa_env();
        std::string status = csrc::exec_output(
            "wpa_cli -p /var/run/wpa_supplicant -i wlan1 status 2>/dev/null");
        std::string ssid;
        size_t pos = status.find("\nssid=");
        if (pos != std::string::npos) {
            size_t e = status.find('\n', pos + 6);
            ssid = status.substr(pos + 6, e == std::string::npos ? std::string::npos : e - pos - 6);
        }
        bool connected = status.find("wpa_state=COMPLETED") != std::string::npos;
        std::string ip = connected ? csrc::iface_ip("wlan1") : "192.168.4.1";
        Json j;
        j["ssid"] = ssid.empty() ? Json() : Json(ssid);
        j["ip"] = ip.empty() ? "192.168.4.1" : ip;
        resp.set_json(j);
    });

    router.add("GET", "/api/wifi/scan", [](const HttpRequest&, HttpResponse& resp, ClientConn&, AppContext&) {
        // 扫描（与 Python get_wifi_list 对齐）
        if (!ensure_wpa_env()) {
            Json j;
            j["list"] = Json(Json::Type::Array);  // 空数组，而非 null
            j["error"] = "WPA_INIT_FAILED";
            resp.set_json(j);
            return;
        }
        csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 scan > /dev/null 2>&1");
        std::string raw;
        for (int i = 0; i < 10; i++) {
            usleep(500000);
            raw = csrc::exec_output(
                "wpa_cli -p /var/run/wpa_supplicant -i wlan1 scan_results 2>/dev/null");
            // 与 Python 一致：等到出现表头以外的至少 1 行结果才退出。
            // 仅表头 "bssid / frequency / ... / ssid\n" 只有 1 个换行，需继续等。
            int nl = 0;
            for (char c : raw) if (c == '\n') ++nl;
            if (nl >= 2) break;
        }
        std::string status = csrc::exec_output(
            "wpa_cli -p /var/run/wpa_supplicant -i wlan1 status 2>/dev/null");
        std::string connected_ssid;
        {
            size_t pos = status.find("\nssid=");
            if (pos != std::string::npos) {
                size_t e = status.find('\n', pos + 6);
                connected_ssid = status.substr(pos + 6, e == std::string::npos ? std::string::npos : e - pos - 6);
            }
        }
        // 解析 scan_results: bssid freq signal flags ssid
        Json list;
        std::istringstream iss(raw);
        std::string line;
        std::map<std::string, Json> unique;
        std::getline(iss, line);  // 表头
        while (std::getline(iss, line)) {
            std::istringstream ls(line);
            std::string bssid, freq, signal_s, flags, ssid;
            ls >> bssid >> freq >> signal_s >> flags;
            std::getline(ls, ssid);
            size_t b = ssid.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            ssid = ssid.substr(b);
            if (ssid.empty()) continue;
            int signal = atoi(signal_s.c_str());
            bool secured = !(flags == "[ESS]" || flags == "[WPS][ESS]");
            if (unique.find(ssid) == unique.end() || signal > (int)unique[ssid].geti("signal", -200)) {
                Json item;
                item["ssid"] = ssid;
                item["id"] = csrc::base64_encode(ssid);
                // base64 去掉 '='
                std::string id = item.gets("id");
                id.erase(std::remove(id.begin(), id.end(), '='), id.end());
                item["id"] = id;
                item["signal"] = csrc::Json((int64_t)signal);
                item["secured"] = secured;
                item["is_connected"] = (ssid == connected_ssid);
                unique[ssid] = item;
            }
        }
        // 排序: 已连接优先, 信号强优先
        std::vector<Json> items;
        for (auto& kv : unique) items.push_back(kv.second);
        std::sort(items.begin(), items.end(), [](const Json& a, const Json& b) {
            if (a.getb("is_connected") != b.getb("is_connected")) return a.getb("is_connected");
            return a.geti("signal") > b.geti("signal");
        });
        for (auto& item : items) list.push_back(item);

        Json j;
        j["list"] = list;
        j["connected"] = connected_ssid.empty() ? Json() : Json(connected_ssid);
        resp.set_json(j);
    });

    router.add("POST", "/api/wifi/connect", [](const HttpRequest& req, HttpResponse& resp, ClientConn&, AppContext&) {
        Json payload = req.json();
        std::string ssid = payload.gets("ssid");
        std::string password = payload.gets("password");
        if (ssid.empty()) {
            resp.set_error("ssid 不能为空", 400);
            return;
        }
        ensure_wpa_env();  // 确保 wlan1 的 wpa_supplicant 已就绪
        // SSID 转 hex（与 Python do_connect 一致，wpa_supplicant 无引号 hex 当字节）
        std::string ssid_hex;
        {
            char buf[4];
            for (unsigned char c : ssid) {
                snprintf(buf, sizeof buf, "%02x", c);
                ssid_hex += buf;
            }
        }
        csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 remove_network all >/dev/null 2>&1");
        std::string add_out = csrc::exec_output(
            "wpa_cli -p /var/run/wpa_supplicant -i wlan1 add_network 2>/dev/null");
        std::string net_id = add_out;
        {
            size_t nl = net_id.find('\n');
            if (nl != std::string::npos) net_id = net_id.substr(0, nl);
            size_t b = net_id.find_first_not_of(" \t\r\n");
            if (b != std::string::npos) net_id = net_id.substr(b);
        }
        if (net_id.empty()) net_id = "0";
        csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 set_network " + net_id +
                          " ssid " + ssid_hex + " >/dev/null 2>&1");
        if (!password.empty()) {
            csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 set_network " + net_id +
                              " psk \"" + password + "\" >/dev/null 2>&1");
        } else {
            csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 set_network " + net_id +
                              " key_mgmt NONE >/dev/null 2>&1");
        }
        csrc::exec_output("wpa_cli -p /var/run/wpa_supplicant -i wlan1 select_network " + net_id +
                          " >/dev/null 2>&1");

        bool ok = false;
        std::string msg = "连接超时";
        for (int attempt = 0; attempt < 10; attempt++) {
            usleep(800000);
            std::string status = csrc::exec_output(
                "wpa_cli -p /var/run/wpa_supplicant -i wlan1 status 2>/dev/null");
            if (status.find("wpa_state=COMPLETED") != std::string::npos) {
                // **不要在这里自己起 DHCP 客户端**。这台板子的 dhcpcd 本来就在管 wlan1：
                // 手动 `ip link set wlan1 up` + wpa_supplicant 时只有一个 IP，正是因为
                // 只有 dhcpcd 在配。capp 再起一个 udhcpc → 两个客户端各要一个地址，
                // 接口上就挂两个 IP（板上实测：dhcpcd 的 .64 + udhcpc 的 .2），
                // 界面显示哪个都不对、用户也不知道哪个能用。
                // 所以这里只等 dhcpcd 关联后自己来配（和手动路径完全一致）。
                std::string ip;
                for (int i = 0; i < 20; i++) {          // 最多等 6s
                    ip = csrc::iface_ip("wlan1");
                    if (!ip.empty()) break;
                    usleep(300000);
                }
                if (ip.empty()) {
                    // 兜底：系统没在跑 dhcpcd（或被配置排除）时，自己拿一次
                    system("udhcpc -i wlan1 -n -q -T 3 >/dev/null 2>&1");
                    ip = csrc::iface_ip("wlan1");
                }
                // 兜底路径可能留下旧地址（udhcpc 只 add 不 del），清一下只留最新的
                ip = csrc::iface_keep_latest_ip("wlan1");
                ok = true;
                msg = ip.empty() ? "获取中..." : ip;
                break;
            }
            if (status.find("FAIL") != std::string::npos ||
                status.find("reason=WRONG_KEY") != std::string::npos ||
                status.find("wpa_state=DISCONNECTED") != std::string::npos ||
                status.find("wpa_state=INACTIVE") != std::string::npos) {
                ok = false;
                msg = "连接失败，请检查密码或信号";
                break;
            }
            if (attempt >= 2 && status.find("wpa_state=SCANNING") != std::string::npos) {
                ok = false;
                msg = "未找到该网络";
                break;
            }
        }
        if (ok) {
            Json j;
            j["ip"] = msg;
            resp.set_json(j);
        } else {
            Json err;
            err["error"] = msg;
            resp.set_json(err, 408);
        }
    });
}
}  // namespace routes
}  // namespace capp
