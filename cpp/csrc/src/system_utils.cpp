// csrc/system_utils.cpp

#include "csrc/system_utils.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/socket.h>
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

std::string detect_local_ip() {
    // 1. wlan1（DHCP dynamic > 最新 > 第一个；udhcpc 现场分配的地址无 dynamic 标志）
    std::string dyn = iface_ip("wlan1");
    if (!dyn.empty()) return dyn;

    // 2. UDP 探测 8.8.8.8:80（wlan1 无 IP 时拿出口网卡地址）
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

    // 3. wlan0（AP 模式默认 192.168.4.1）/ 兜底
    std::string ip = iface_ip("wlan0");
    if (!ip.empty()) return ip;
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

}  // namespace csrc
