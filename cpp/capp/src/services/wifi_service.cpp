// WiFi STA 服务（wlan1 → 目标路由器）
//
// 对应 app/routes/wifi.py 里"wpa_supplicant 自举"那部分；HTTP 那一层留在
// routes/wifi.cpp。声明在 capp/context.hpp。
//
// 这里多做一件 Python 版没有的事：**把最后一次成功连接的 WiFi 记下来**，
// capp 下次启动时在后台重放一遍，用户不用每次开机都在界面里重连。
//
// 为什么不走 wpa_supplicant 自己的 save_config（那才是"正统"做法）：
//   save_config 要求 wpa_supplicant 是**带 `-c <配置文件>` 启动的**且配置里有
//   `update_config=1`。而板上 wlan1 的 wpa_supplicant 是 capp 自举的
//   （init_ap_web.sh 的 S99webstart 只 `ip link set wlan1 up`，没有任何启动脚本起它），
//   所以走那条路要同时改 ensure_wpa_env + 板子上的启动脚本两处，还把
//   "配置文件写坏 → wpa_supplicant 起不来 → wlan1 彻底死掉"引成一个新的致命失败模式。
//   本方案不动启动方式，且 select_network 之后 wpa_supplicant 本来就会持续重试，
//   效果等价。

#include "capp/context.hpp"

#include "csrc/json.hpp"
#include "csrc/log.hpp"
#include "csrc/system_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

namespace capp {

namespace {

const char* kIface   = "wlan1";
const char* kCtrlDir = "/var/run/wpa_supplicant";

// 串行化"改 wpa_supplicant 网络表"的那几条命令。
//
// **必须加**：HTTP 是 thread-per-connection（src/http/server.cpp 里 detach），
// 用户点"连接"和启动重放线程会同时发 remove_network all / add_network，
// 两条序列交叉的后果是网络表被搅成"ssid 是 A、psk 是 B"这种四不像。
std::mutex g_wpa_mu;

std::string wpa_cli(const std::string& args) {
    return csrc::exec_output(std::string("wpa_cli -p ") + kCtrlDir + " -i " + kIface + " " +
                             args + " 2>/dev/null");
}

/// 取 stdout 的第一行并去掉首尾空白（wpa_cli 的行命令返回形如 "0\n"）
std::string first_line_trimmed(const std::string& s) {
    std::string t = s;
    size_t nl = t.find('\n');
    if (nl != std::string::npos) t = t.substr(0, nl);
    size_t b = t.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = t.find_last_not_of(" \t\r\n");
    return t.substr(b, e - b + 1);
}

/// 把字符串包成 shell 的单引号字面量（内部的单引号走经典的 '\'' 拼接）。
///
/// 为什么需要：几条 wpa_cli 命令是拼成字符串交给 popen 的。密码里只要有一个 `$`
/// 或反引号就会被 shell 展开 —— 连不上还是小事，**这等于以 root 身份执行注入**。
/// 单引号里的字符 shell 一律不解析，最省心。
/// （这条以前只是"用户点一次连接"，现在密码会被存下来、每次开机重放一次，
///  所以必须挡。）
std::string shell_squote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

}  // namespace

std::string wifi_cred_path() {
    // 本机调试用：不设就走板子上的真路径。有它才能在开发机上验"读得到/读不到"，
    // 而不必往开发机的 /etc 里写一个真的 wifi 密码文件。
    const char* p = getenv("AKA_WIFI_CONF");
    return (p && *p) ? p : "/etc/aka-wifi.json";
}

bool wifi_save_credential(const std::string& ssid, const std::string& password) {
    if (ssid.empty()) return false;
    // 白名单重建（照 routes/config.cpp 的 speed_config 模板）：不把外部来的对象直接 dump
    csrc::Json j;
    j["ssid"] = ssid;
    j["password"] = password;
    // 0600：文件里有明文密码。write_file_atomic 内部 fchmod，不受 umask 影响。
    return csrc::write_file_atomic(wifi_cred_path(), j.dump(false), 0600);
}

bool wifi_load_credential(std::string& ssid, std::string& password) {
    std::ifstream f(wifi_cred_path());
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.empty()) return false;             // 0 字节也算"没存过"

    csrc::Json j;
    if (!csrc::Json::parse(text, j) || !j.is_object()) return false;
    ssid = j.gets("ssid");
    password = j.gets("password");
    if (ssid.empty()) return false;
    // 注意：这里**不删文件**。读不出来就留着让人看得见（可能就是被断电写坏的那个），
    // 删掉反而把现场毁了。
    return true;
}

bool ensure_wpa_env() {
    const std::string ctrl = kCtrlDir;
    const std::string iface = kIface;
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

bool wifi_apply_network(const std::string& ssid, const std::string& password) {
    std::lock_guard<std::mutex> lk(g_wpa_mu);

    // SSID 转 hex（与 Python do_connect 一致，wpa_supplicant 无引号 hex 当字节）
    std::string ssid_hex;
    {
        char buf[4];
        for (unsigned char c : ssid) {
            snprintf(buf, sizeof buf, "%02x", c);
            ssid_hex += buf;
        }
    }
    wpa_cli("remove_network all >/dev/null 2>&1");
    const std::string net_id = first_line_trimmed(wpa_cli("add_network"));
    if (net_id.empty()) return false;      // 拿不到 id = wpa_supplicant 没在干活
    wpa_cli("set_network " + net_id + " ssid " + ssid_hex + " >/dev/null 2>&1");
    if (!password.empty()) {
        // 外面再套一层双引号**是故意的**：让 wpa_cli 把引号原样转给 wpa_supplicant，
        // 那边才会按"口令"解析（长度正好 64 的密码不会被误当成已经是 hash 的 PSK）。
        wpa_cli("set_network " + net_id + " psk " +
                shell_squote("\"" + password + "\"") + " >/dev/null 2>&1");
    } else {
        wpa_cli("set_network " + net_id + " key_mgmt NONE >/dev/null 2>&1");
    }
    wpa_cli("select_network " + net_id + " >/dev/null 2>&1");
    // 返回 true 只代表命令下发成功，**不代表连上了** —— 等待与判定由调用方自己来
    // （两者的判据不一样，见 start_wifi_autoconnect 的注释）。
    return true;
}

std::string wifi_wpa_state() {
    const std::string status = wpa_cli("status");
    size_t pos = status.find("wpa_state=");
    if (pos == std::string::npos) return "";
    pos += 10;
    size_t e = status.find('\n', pos);
    return status.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
}

std::string wifi_current_ssid() {
    const std::string status = wpa_cli("status");
    size_t pos = status.find("\nssid=");
    if (pos == std::string::npos) return "";
    pos += 6;
    size_t e = status.find('\n', pos);
    return status.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
}

void start_wifi_autoconnect() {
    // detach 且**不捕获 ctx**：线程体只碰常量路径、日志、popen。所以 main 的退出清理
    // 段不需要 join 它，也不存在"main 返回后线程摸已析构的 ctx"的悬垂问题 ——
    // **别往这个线程里加 AppContext 引用**，加了这两条就都不成立了。
    std::thread([] {
        std::string ssid, password;
        if (!wifi_load_credential(ssid, password)) {
            // 这里**直接返回，不调 ensure_wpa_env()** —— 从没用过 STA 的板子不该
            // 白起一个 wpa_supplicant、白把 wlan1 拉起来。
            CAM_INFO("[wifi] 没有已保存的 WiFi，跳过自动重连");
            return;
        }
        CAM_INFO("[wifi] 自动重连：读到上次连接 %s", ssid.c_str());

        // 顺序不能反：wpa_supplicant 是 capp 自举的，capp 刚起来时它多半还没起，
        // 这时查 status 只会拿到空串、被误判成"没连上"。
        if (ensure_wpa_env()) {
            // ensure_wpa_env 只看 socket 文件在不在（进程崩了 socket 可能残留），
            // 拿 ping 探一下活，免得后面失败时日志里一点线索都没有。
            if (first_line_trimmed(wpa_cli("ping")) != "PONG") {
                CAM_WARN("[wifi] wpa_supplicant 控制接口没应答，自动重连放弃");
                return;
            }
            if (wifi_wpa_state() == "COMPLETED") {
                // capp 崩溃重启 / OTA 换包后 wpa_supplicant 还活着且连着 ——
                // 别去动它（重放会先 remove_network all，白白掉一次线）。
                // 注意判据只看状态、不比对 SSID：此时连的可能是用户手连的另一个网，
                // 也不该打断他。
                CAM_INFO("[wifi] wlan1 已连接 (%s)，跳过自动重连", wifi_current_ssid().c_str());
                return;
            }
        }

        bool applied = false;
        for (int i = 0; i < 3 && !applied; i++) {
            if (ensure_wpa_env() && wifi_apply_network(ssid, password)) {
                applied = true;
                break;
            }
            sleep(5);   // 网卡/守护还没就绪 —— 只有这种"下发不进去"才值得重试
        }
        if (!applied) {
            CAM_WARN("[wifi] 自动重连：网络下发失败（wpa_supplicant 不可用）");
            return;
        }

        // 下面这段等待**纯粹是为了日志**。真正干活的是上面那条 select_network ——
        // 网络一旦选中，wpa_supplicant 自己会无限重试（路由器不在范围就先 SCANNING，
        // 回来了自己连上），所以这里等不到也不会怎样，**不要**在这条路径上做
        // "失败就删凭据 / 失败就 remove_network all" 之类的事。
        //
        // 也**不能**照搬 /api/wifi/connect 里那套失败判定：那边第一轮 800ms 就可能看到
        // DISCONNECTED/INACTIVE 并判"连接失败"，而那恰恰是 select_network 之后的正常
        // 中间态（它靠 `attempt>=2 && SCANNING` 的特判绕开了这个坑）。这里只等
        // COMPLETED，别的一概不管。
        for (int i = 0; i < 25 && wifi_wpa_state() != "COMPLETED"; i++) usleep(800000);
        if (wifi_wpa_state() != "COMPLETED") {
            CAM_WARN("[wifi] 自动重连：20s 内没关联上 %s（不在范围或密码不对），后台继续重试",
                     ssid.c_str());
            return;
        }
        // 等 dhcpcd 配 IP。**不自己起 DHCP 客户端** —— 那正是 csrc/system_utils.hpp 里
        // 那段注释记录的坑（两个客户端各拿一个地址，界面上显示哪个都不对）。
        for (int i = 0; i < 50 && csrc::iface_ip(kIface).empty(); i++) usleep(300000);
        const std::string ip = csrc::iface_keep_latest_ip(kIface);
        if (ip.empty()) {
            CAM_WARN("[wifi] 已关联上 %s 但没拿到 IP —— dhcpcd 还在管 wlan1 吗？", ssid.c_str());
        } else {
            CAM_INFO("[wifi] 自动重连成功：%s ip=%s", ssid.c_str(), ip.c_str());
        }
    }).detach();
}

}  // namespace capp
