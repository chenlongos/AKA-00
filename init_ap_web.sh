#!/bin/bash
# =============================================================================
# AKA-00 AP 热点 + Web 服务初始化 (RK3576 / Debian 12)
#
# 用法:
#   ./init_ap_web.sh             # 自动模式: wlan1 存在时 wlan0=AP + wlan1=STA,
#                                #            wlan1 不存在时仅 wlan0=AP
#   ./init_ap_web.sh --ap        # 仅 wlan0 开启 AP
#   ./init_ap_web.sh --sta       # 仅 wlan0 开启 STA
#   ./init_ap_web.sh --silent    # 静默模式（供 systemd ExecStartPre 调用）
#   参数可组合，如: ./init_ap_web.sh --ap --silent
#
# 说明:
#   - 使用 hostapd + dnsmasq + wpa_supplicant
#   - AP 角色: hostapd + dnsmasq / STA 角色: wpa_supplicant + dhclient
#   - AP 与 STA 同时启动时，AP 先启动，10 秒后再启动 STA
#   - hostapd 配置:       <脚本目录>/services/hostapd.conf (模板, 运行时动态生成 SSID/信道)
#   - wpa_supplicant 配置: <脚本目录>/services/wpa_supplicant.conf
# =============================================================================

set -e

usage() {
    echo "用法: $0 [--ap | --sta] [--silent]"
    echo "  (无参数)   自动模式: wlan1 存在时 wlan0=AP + wlan1=STA, 否则仅 wlan0=AP"
    echo "  --ap      仅 wlan0 开启 AP"
    echo "  --sta     仅 wlan0 开启 STA"
    echo "  --silent  静默模式（抑制日志输出）"
}

# ── 解析参数 ────────────────────────────────────────────────────────────────
AP_FLAG=false
STA_FLAG=false
SILENT=false

for arg in "$@"; do
    case "$arg" in
        --ap)     AP_FLAG=true ;;
        --sta)    STA_FLAG=true ;;
        --silent) SILENT=true ;;
        *)        echo "未知参数: ${arg}" >&2; usage >&2; exit 1 ;;
    esac
done

if $AP_FLAG && $STA_FLAG; then
    echo "❌ --ap 与 --sta 不能同时使用（都作用于 wlan0）" >&2
    exit 1
fi

if $SILENT; then
    exec 2>/dev/null
fi

log() { $SILENT || echo "$@"; }
err() { echo "❌ $@" >&2; }

# ── 解析脚本所在目录（支持从任意工作目录调用） ──────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"

# ── 接口与服务配置 ──────────────────────────────────────────────────────────
AP_IFACE="wlan0"
STA_IFACE="wlan1"
AP_ADDR="192.168.4.1"
AP_PREFIX="24"
DHCP_RANGE_START="192.168.4.10"
DHCP_RANGE_END="192.168.4.100"
DHCP_LEASE_TIME="12h"

HOSTAPD_TEMPLATE="${SCRIPT_DIR}/services/hostapd.conf"
WPA_SUPPLICANT_CONF="${SCRIPT_DIR}/services/wpa_supplicant.conf"
HOSTAPD_RUNTIME="/tmp/hostapd_runtime.conf"

# ── 决定角色分配 ────────────────────────────────────────────────────────────
# RUN_AP/RUN_STA 决定要启动的服务; STA_TARGET 为承担 STA 角色的接口
RUN_AP=false
RUN_STA=false
STA_TARGET=""

if $AP_FLAG; then
    # 显式 --ap: 仅 wlan0 开启 AP
    RUN_AP=true
elif $STA_FLAG; then
    # 显式 --sta: 仅 wlan0 开启 STA
    RUN_STA=true
    STA_TARGET="$AP_IFACE"
elif [ -d "/sys/class/net/${STA_IFACE}" ]; then
    # 自动模式: wlan1 存在 → wlan0=AP + wlan1=STA
    RUN_AP=true
    RUN_STA=true
    STA_TARGET="$STA_IFACE"
else
    # 自动模式: wlan1 不存在 → 仅 wlan0=AP
    RUN_AP=true
fi

log "角色分配: AP=${RUN_AP} STA=${RUN_STA}${STA_TARGET:+(接口: ${STA_TARGET})}"

# ── 检测是否需要 sudo ─────────────────────────────────────────────────────────
SUDO=""
if [ "$(id -u)" != "0" ]; then
    if sudo -n true 2>/dev/null; then
        SUDO="sudo"
    else
        err "需要 root 或 sudo 权限（hostapd/dnsmasq/wpa_supplicant 需要 root）"
        exit 1
    fi
fi

# ── 检查模板配置文件（按实际角色检查） ────────────────────────────────────────
if $RUN_AP && [ ! -f "$HOSTAPD_TEMPLATE" ]; then
    err "hostapd 模板配置文件不存在: ${HOSTAPD_TEMPLATE}"
    exit 1
fi
if $RUN_STA && [ ! -f "$WPA_SUPPLICANT_CONF" ]; then
    err "wpa_supplicant 配置文件不存在: ${WPA_SUPPLICANT_CONF}"
    exit 1
fi

# ── AP 角色启动 (hostapd + dnsmasq) ─────────────────────────────────────────
start_ap() {
    local iface="$AP_IFACE"

    # 生成 SSID (基于接口 MAC) 和随机信道
    log "生成 SSID 和信道配置..."
    local mac_suffix
    mac_suffix=$(cat "/sys/class/net/${iface}/address" 2>/dev/null | tr -d ':' | tail -c 6)
    if [ -z "${mac_suffix}" ]; then
        err "无法读取 ${iface} MAC 地址"
        exit 1
    fi
    local random_id=$((16#${mac_suffix} % 9999999 + 1))
    SSID="chenlong-robot-${random_id}"

    local channels=(1 6 11)
    local channel=${channels[$((RANDOM % ${#channels[@]}))]}
    log "  SSID: ${SSID} (MAC suffix: ${mac_suffix})"
    log "  信道: ${channel}"

    # 基于模板生成运行时 hostapd 配置（不修改原模板）
    cp "$HOSTAPD_TEMPLATE" "$HOSTAPD_RUNTIME"
    sed -i "s/^ssid=.*/ssid=${SSID}/" "$HOSTAPD_RUNTIME"
    sed -i "s/^channel=.*/channel=${channel}/" "$HOSTAPD_RUNTIME"

    # 配置接口 (AP)
    log "配置 ${iface} 接口 (AP)..."
    $SUDO ip link set "$iface" down
    $SUDO ip addr flush dev "$iface"
    $SUDO ip addr add "${AP_ADDR}/${AP_PREFIX}" dev "$iface"
    $SUDO ip link set "$iface" up

    # 启动 dnsmasq (DHCP 服务)
    log "启动 dnsmasq (DHCP)..."
    $SUDO dnsmasq \
        --interface="$iface" \
        --bind-interfaces \
        --except-interface=lo \
        --dhcp-range="${DHCP_RANGE_START},${DHCP_RANGE_END},${DHCP_LEASE_TIME}" \
        --dhcp-option=3,"${AP_ADDR}" \
        --dhcp-option=6,"${AP_ADDR}"

    # 启动 hostapd (AP 热点)
    log "启动 hostapd (AP 热点)..."
    $SUDO hostapd -B "$HOSTAPD_RUNTIME"

    log ""
    log "=========================================="
    log "  ✅ AP 热点已启动"
    log "  接口:     ${iface}"
    log "  SSID:     ${SSID}"
    log "  IP:       ${AP_ADDR}"
    log "  连接后访问: http://${AP_ADDR}"
    log "=========================================="
}

# ── STA 角色启动 (wpa_supplicant + dhclient) ─────────────────────────────────
start_sta() {
    local iface="$1"

    log "启动 wpa_supplicant (STA: ${iface})..."

    # 确保控制接口目录存在且属于 netdev 组（配置文件中 GROUP=netdev 依赖此设置）
    $SUDO mkdir -p /var/run/wpa_supplicant
    $SUDO chown root:netdev /var/run/wpa_supplicant 2>/dev/null || $SUDO chown root:root /var/run/wpa_supplicant
    $SUDO chmod 775 /var/run/wpa_supplicant

    $SUDO ip link set "$iface" up
    $SUDO wpa_supplicant -B -i "$iface" -c "$WPA_SUPPLICANT_CONF"
    $SUDO dhclient "$iface"

    log ""
    log "=========================================="
    log "  ✅ STA 已启动"
    log "  接口:     ${iface}"
    log "  配置:     ${WPA_SUPPLICANT_CONF}"
    log "=========================================="
}

# ── 1. 清理已有进程 ──────────────────────────────────────────────────────────
log "清理已有的 hostapd / dnsmasq / wpa_supplicant 进程..."
$SUDO killall wpa_supplicant 2>/dev/null || true
$SUDO killall hostapd 2>/dev/null || true
$SUDO killall dnsmasq 2>/dev/null || true
sleep 1

# ── 2. 按角色启动服务 ────────────────────────────────────────────────────────
if $RUN_AP; then
    start_ap
fi

if $RUN_STA; then
    # AP 与 STA 同时启动时，等待 10 秒避免抢占
    if $RUN_AP; then
        log "等待 10 秒后启动 STA (${STA_TARGET})..."
        sleep 10
    fi
    start_sta "$STA_TARGET"
fi
