// csrc/system_utils.hpp — 系统信息工具
//
// 对应 app/routes/_utils.py + app/services/status_reporter.py：
//   - 本机 IP（UDP 探测 8.8.8.8 → wlan1 → wlan0 → 127.0.0.1）
//   - MAC（/sys/class/net/wlan0/address）
//   - CPU / 内存 / 磁盘 / 运行时长（/proc + statvfs）

#pragma once

#include <string>

namespace csrc {

/// 本机 IP，优先级：wlan1（DHCP 优先）→ UDP 探测 → wlan0 → 127.0.0.1
std::string detect_local_ip();

/// 指定网卡 IPv4（`ip -4 -o addr show` 解析）。
/// 多 IP 时优先级：dynamic（DHCP）> 最后一个（最新）> 第一个。
std::string iface_ip(const std::string& ifname);

/// MAC 地址（/sys/class/net/<iface>/address），失败 "unknown"
std::string mac_address(const std::string& ifname = "wlan0");

/// CPU 使用率 %（/proc/stat）
int cpu_usage();
/// 内存使用率 %（/proc/meminfo）
int mem_usage();
/// 磁盘使用率 %（statvfs /）
int disk_usage();
/// 运行时长秒（/proc/uptime）
int uptime_secs();

/// 读文件内容（trim 后），失败返回空串
std::string read_sys_file(const std::string& path);

/// 执行命令并返回 stdout（popen），失败返回空串
std::string exec_output(const std::string& cmd);

}  // namespace csrc
