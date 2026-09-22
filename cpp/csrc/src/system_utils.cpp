// csrc/system_utils.cpp

#include "csrc/system_utils.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace csrc {

std::string read_sys_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool write_file_atomic(const std::string& path, const std::string& content, int mode) {
    const std::string tmp = path + ".part";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    // umask 会削掉 mode 里的位，显式 fchmod 才能保证最终权限就是调用方要的那个
    // （放密码的文件要 0600，光靠 open 的 mode 参数在 umask=022 下会变成 0600&~022=0600，
    //  但 umask=077 之类又会削别的位 —— 不依赖 umask，写死）。
    if (fchmod(fd, mode) != 0) {
        close(fd);
        unlink(tmp.c_str());
        return false;
    }
    size_t off = 0;
    while (off < content.size()) {
        ssize_t n = write(fd, content.data() + off, content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        if (n == 0) {   // 理论上不会发生；真发生了就是写不动了，别死循环
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        off += (size_t)n;
    }
    // 顺序不能换：先 fsync 落盘、再 rename 换入。反过来在断电时可能留下 0 字节的最终文件。
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp.c_str());
        return false;
    }
    if (close(fd) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::string exec_output(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
        out.append(buf, n);
        if (out.size() > 65536) break;
    }
    pclose(p);
    return out;
}

std::string iface_ip(const std::string& ifname) {
    std::string out = exec_output("ip -4 -o addr show " + ifname + " 2>/dev/null");
    // 行形如:
    //   4: wlan1    inet 172.16.203.94/24 brd ... scope global wlan1
    //   4: wlan1    inet 172.16.203.195/24 brd ... scope global secondary dynamic wlan1
    // 网卡可能同时有多个 IP：static primary（旧配置，可能已失效）+ DHCP 新地址。
    // 优先级: dynamic（系统 DHCP 带标志）> 最后一个（最新添加；udhcpc 现场分配
    // 的地址不带 dynamic 标志，但一定排在输出末尾）> 第一个。
    std::string first_ip, dynamic_ip, last_ip;
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        size_t pos = line.find("inet ");
        if (pos == std::string::npos) continue;
        pos += 5;
        size_t end = line.find('/', pos);
        if (end == std::string::npos) continue;
        std::string ip = line.substr(pos, end - pos);
        size_t b = ip.find_first_not_of(" \t");
        if (b != std::string::npos) ip = ip.substr(b);
        size_t e = ip.find_last_not_of(" \t");
        if (e != std::string::npos) ip = ip.substr(0, e + 1);
        if (ip.empty()) continue;
        if (first_ip.empty()) first_ip = ip;
        last_ip = ip;
        if (line.find("dynamic") != std::string::npos) dynamic_ip = ip;
    }
    if (!dynamic_ip.empty()) return dynamic_ip;
    if (!last_ip.empty()) return last_ip;
    return first_ip;
}

/// 网卡上带 `dynamic` 标志的 IPv4（DHCP 分配），无则空。
/// static primary（如残留的 .94）不算 —— DHCP 未完成时不能把它当可用地址。
static std::string iface_dynamic_ip(const std::string& ifname) {
    std::string out = exec_output("ip -4 -o addr show " + ifname + " 2>/dev/null");
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("dynamic") == std::string::npos) continue;
        size_t pos = line.find("inet ");
        if (pos == std::string::npos) continue;
        pos += 5;
        size_t end = line.find('/', pos);
        if (end == std::string::npos) continue;
        std::string ip = line.substr(pos, end - pos);
        size_t b = ip.find_first_not_of(" \t");
        if (b != std::string::npos) ip = ip.substr(b);
        size_t e = ip.find_last_not_of(" \t");
        if (e != std::string::npos) ip = ip.substr(0, e + 1);
        if (!ip.empty()) return ip;
    }
    return "";
}

/// 网卡全部 IPv4（按 `ip addr` 输出顺序）
static std::vector<std::string> iface_ip_list(const std::string& ifname) {
    std::vector<std::string> out;
    std::string s = exec_output("ip -4 -o addr show " + ifname + " 2>/dev/null");
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        size_t pos = line.find("inet ");
        if (pos == std::string::npos) continue;
        pos += 5;
        size_t end = line.find('/', pos);
        if (end == std::string::npos) continue;
        std::string ip = line.substr(pos, end - pos);
        size_t b = ip.find_first_not_of(" \t");
        if (b != std::string::npos) ip = ip.substr(b);
        size_t e = ip.find_last_not_of(" \t");
        if (e != std::string::npos) ip = ip.substr(0, e + 1);
        if (!ip.empty()) out.push_back(ip);
    }
    return out;
}

std::string iface_keep_latest_ip(const std::string& ifname) {
    auto addrs = iface_ip_list(ifname);
    if (addrs.empty()) return "";
    const std::string keep = addrs.back();   // udhcpc 新加的排在最后
    for (size_t i = 0; i + 1 < addrs.size(); i++) {
        if (addrs[i] == keep) continue;
        exec_output("ip addr del " + addrs[i] + "/24 dev " + ifname + " 2>/dev/null");
    }
    return keep;
}

std::string detect_local_ip() {
    // 1. wlan1 的 DHCP dynamic 地址（STA 场景：DHCP 完成后的正确 IP，如 .195）
    std::string dyn = iface_dynamic_ip("wlan1");
    if (!dyn.empty()) return dyn;

    // 2. wlan1 无 dynamic：
    //    - 多个地址 → 取最后一个（udhcpc 现场分配的地址无 dynamic 标志但排在末尾）
    //    - 只有一个地址（疑似过期的 static，如 .94）→ 返回空，前端保持"获取中..."
    //      —— 用户宁可多等也不要错的 IP
    auto addrs = iface_ip_list("wlan1");
    if (addrs.size() > 1) return addrs.back();
    if (addrs.size() == 1) return "";

    // 3. wlan1 完全无地址：AP 模式优先 wlan0（192.168.4.1）
    std::string w0 = iface_ip("wlan0");
    if (!w0.empty()) return w0;

    // 4. UDP 探测 8.8.8.8:80 / 兜底
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        sockaddr_in sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(80);
        inet_pton(AF_INET, "8.8.8.8", &sa.sin_addr);
        if (connect(fd, (sockaddr*)&sa, sizeof sa) == 0) {
            sockaddr_in local;
            socklen_t len = sizeof local;
            if (getsockname(fd, (sockaddr*)&local, &len) == 0) {
                char ip[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &local.sin_addr, ip, sizeof ip);
                close(fd);
                if (ip[0] && strcmp(ip, "0.0.0.0") != 0) return ip;
                return ip;
            }
        }
        close(fd);
    }
    return "127.0.0.1";
}

std::string mac_address(const std::string& ifname) {
    std::string mac = read_sys_file("/sys/class/net/" + ifname + "/address");
    return mac.empty() ? "unknown" : mac;
}

int cpu_usage() {
    std::string s = read_sys_file("/proc/stat");
    if (s.empty()) return 0;
    // cpu  user nice system idle iowait irq softirq steal ...
    std::istringstream iss(s);
    std::string tag;
    long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    iss >> tag >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
    if (tag != "cpu") return 0;
    long long total = user + nice + sys + idle + iowait + irq + softirq + steal;
    if (total <= 0) return 0;
    long long idle_all = idle + iowait;
    int pct = (int)((1.0 - (double)idle_all / total) * 100.0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

int mem_usage() {
    std::string s = read_sys_file("/proc/meminfo");
    if (s.empty()) return 0;
    long long total = 0, available = 0;
    std::istringstream iss(s);
    std::string key, unit;
    long long val;
    while (iss >> key >> val >> unit) {
        if (key == "MemTotal:") total = val;
        else if (key == "MemAvailable:") available = val;
        else if (key == "MemFree:" && available == 0) available = val;
    }
    if (total <= 0) return 0;
    int pct = (int)((1.0 - (double)available / total) * 100.0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

int disk_usage() {
    struct statvfs v;
    if (statvfs("/", &v) != 0 || v.f_blocks <= 0) return 0;
    double used = 1.0 - (double)v.f_bavail / v.f_blocks;
    int pct = (int)(used * 100.0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

int uptime_secs() {
    std::string s = read_sys_file("/proc/uptime");
    if (s.empty()) return 0;
    return (int)strtod(s.c_str(), nullptr);
}

bool ensure_dir(const std::string& path) {
    if (path.empty()) return false;
    // 逐级建："a/b/c" → "a"、"a/b"、"a/b/c"。EEXIST 不算失败（并发/重复调用都可能撞上）。
    for (size_t i = 1; i <= path.size(); i++) {
        if (i != path.size() && path[i] != '/') continue;
        const std::string part = path.substr(0, i);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

}  // namespace csrc
