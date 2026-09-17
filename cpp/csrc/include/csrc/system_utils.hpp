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

/// 把接口上**除最新一个之外**的 IPv4 地址都删掉，返回保留的那个。
/// 背景：capp 的 /api/wifi/connect 里原本自己起了一个 udhcpc，而系统本来就有 dhcpcd
/// 在管 wlan1 —— 两个 DHCP 客户端各拿一个地址，接口上于是挂着两个 IP（板上实测
/// 172.16.203.64 来自 dhcpcd、172.16.203.2 来自 udhcpc），界面显示哪个都不对。
/// 那条多余的 udhcpc 已经去掉（改成等 dhcpcd 配），这个函数留作安全网：兜底路径
/// （系统没有 dhcpcd 时才走）以及历史遗留都可能留下多余地址。
/// 保留最新的那个而不是"先清空再要"，是为了清理过程中连接不中断；只有一个地址时是空操作。
std::string iface_keep_latest_ip(const std::string& ifname);

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
