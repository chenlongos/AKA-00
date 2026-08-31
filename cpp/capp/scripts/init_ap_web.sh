#!/bin/sh
# =============================================================================
# AKA-00 AP 热点 + capp 开机自启配置脚本（SG2002 / HD05085A 板子）
#
# 对应 Python 版 init_ap_web.sh：开机自启一个 AP（wlan0），手机/控制器连上热点后
# 浏览器访问 http://192.168.4.1 即可控制机器人；wlan1 作为 STA 由 capp 扫描并
# 连接目标路由器（capp 的 /api/wifi/scan、/api/wifi/connect 走 wlan1）。
#
# 与 Python 版的差异（适配本板）：
#   1. 本板无 udhcpd，改用 dnsmasq 做 DHCP（已验证 dnsmasq 2.89 可用）。
#   2. 只 terminate wlan0 的 wpa_supplicant，不 killall，避免误杀 capp 在 wlan1
#      上自举的 wpa_supplicant（那是用户连目标路由器用的）。
#   3. capp 在 S99 里用 `init.sh &` 后台启动而非 `exec`，避免 rcS 卡在 sysinit，
#      导致 getty / start_app.sh 不启动（本板 capp 原本是控制台手动启动的）。
#
# 用法:
#   ./init_ap_web.sh             # 安装配置 + 立即启动 AP（会断开 wlan0 当前 STA 连接）
#   ./init_ap_web.sh install     # 只安装配置和开机脚本，不改动当前网络（下次 reboot 生效）
#   ./init_ap_web.sh start       # 只立即启动 AP（已安装配置时使用）
# =============================================================================

AP_IFACE="${AP_IFACE:-wlan0}"          # AP 接口（给手机/控制器连）
STA_IFACE="${STA_IFACE:-wlan1}"        # STA 接口（capp 用来连目标路由器）
AP_IP="${AP_IP:-192.168.4.1}"
NETMASK="${NETMASK:-255.255.255.0}"
DHCP_START="${DHCP_START:-192.168.4.100}"
DHCP_END="${DHCP_END:-192.168.4.200}"
CHANNEL="${CHANNEL:-6}"
APP_DIR="${AKA_HOME:-/root/AKA-00}"

MODE="${1:-all}"   # all | install | start

# ── 1. 基于 wlan0 MAC 生成唯一 SSID（与 Python 版一致）──
MAC_SUFFIX=$(cat /sys/class/net/${AP_IFACE}/address 2>/dev/null | tr -d ':' | tail -c 6)
[ -z "$MAC_SUFFIX" ] && MAC_SUFFIX="123456"
RANDOM_ID=$((16#${MAC_SUFFIX} % 9999999 + 1))
SSID="chenlong-robot-${RANDOM_ID}"

# ── 2. hostapd 配置（开放热点，与 Python 版一致；如需 WPA2 见注释）──
cat > /etc/hostapd.conf <<EOF
interface=${AP_IFACE}
driver=nl80211
ssid=${SSID}
hw_mode=g
channel=${CHANNEL}
auth_algs=1
# 如需 WPA2 加密，取消下面 4 行注释并设置密码：
#wpa=2
#wpa_key_mgmt=WPA-PSK
#wpa_pairwise=CCMP
#wpa_passphrase=your_password_here
EOF

# ── 3. DHCP 配置（dnsmasq；本板无 udhcpd）──
cat > /etc/dnsmasq.ap.conf <<EOF
interface=${AP_IFACE}
bind-interfaces
dhcp-range=${DHCP_START},${DHCP_END},${NETMASK},12h
dhcp-option=3,${AP_IP}
dhcp-option=6,${AP_IP}
EOF

# ── 4. 开机脚本 S98apstart：等 wlan0 就绪 → 起 AP + DHCP ──
cat > /etc/init.d/S98apstart <<EOF
#!/bin/sh
# 等 AP 接口就绪（轮询，最多 20s）
i=0
while [ \$i -lt 40 ]; do
    [ -d /sys/class/net/${AP_IFACE} ] && break
    sleep 0.5
    i=\$((i+1))
done

# 只 terminate wlan0 的 wpa_supplicant（保护 capp 在 wlan1 上自举的那个）
wpa_cli -p /var/run/wpa_supplicant -i ${AP_IFACE} terminate 2>/dev/null
killall hostapd dnsmasq udhcpd 2>/dev/null
sleep 1

ifconfig ${AP_IFACE} ${AP_IP} netmask ${NETMASK} up
dnsmasq --conf-file=/etc/dnsmasq.ap.conf --pid-file=/var/run/dnsmasq.ap.pid &
hostapd -B /etc/hostapd.conf
exit 0
EOF
chmod 755 /etc/init.d/S98apstart

# ── 5. 开机脚本 S99webstart：建/起 wlan1 → 后台启动 capp ──
cat > /etc/init.d/S99webstart <<EOF
#!/bin/sh
# 等 wifi phy 就绪（轮询，最多 30s）
i=0
while [ \$i -lt 60 ]; do
    iw dev 2>/dev/null | grep -q "phy#" && break
    sleep 0.5
    i=\$((i+1))
done

# 防御：S99init/hdzk_init.sh 可能在 S98apstart 之后又把 wlan0 拉成 STA（HD05070A
# 板会调 hdzk_init.sh；本板默认不会，但万一被改）。这里在 S99init 之后重新夺回 AP：
# 只 terminate wlan0 的 wpa_supplicant（不动 wlan1 上 capp 自举的那个），再确保
# hostapd/dnsmasq 在跑。
wpa_cli -p /var/run/wpa_supplicant -i ${AP_IFACE} terminate 2>/dev/null
ifconfig ${AP_IFACE} ${AP_IP} netmask ${NETMASK} up
pidof hostapd >/dev/null 2>&1 || hostapd -B /etc/hostapd.conf
if ! pidof dnsmasq >/dev/null 2>&1; then
    dnsmasq --conf-file=/etc/dnsmasq.ap.conf --pid-file=/var/run/dnsmasq.ap.pid &
fi

# 确保 wlan1 存在（STA 接口，capp 扫描/连接目标路由器用）
if ! iw dev 2>/dev/null | grep -q "${STA_IFACE}"; then
    iw phy phy0 interface add ${STA_IFACE} type managed
    sleep 1
fi
ip link set ${STA_IFACE} up 2>/dev/null

# 后台启动 capp（init.sh 自带自愈循环 + 防重复实例）。用 & 而非 exec，避免 rcS
# 卡在 sysinit 导致 getty / start_app.sh 不启动。
chmod +x ${APP_DIR}/init.sh
( AKA_HOME=${APP_DIR} ${APP_DIR}/init.sh ) &
exit 0
EOF
chmod 755 /etc/init.d/S99webstart

echo "✅ 已安装:"
echo "   /etc/hostapd.conf           (AP: ${SSID}, 开放, channel ${CHANNEL})"
echo "   /etc/dnsmasq.ap.conf        (DHCP: ${DHCP_START}~${DHCP_END})"
echo "   /etc/init.d/S98apstart      (开机: wlan0 → AP)"
echo "   /etc/init.d/S99webstart     (开机: wlan1 起 + capp 自启)"
echo "✅ 访问地址: http://${AP_IP}    STA 接口: ${STA_IFACE} (capp 连目标路由器)"

case "$MODE" in
  install)
    echo "ℹ 仅安装，未改动当前网络。reboot 后生效：wlan0 变 AP，访问 http://${AP_IP}"
    exit 0
    ;;
  start)
    ;;
  all|"")
    ;;
  *)
    echo "用法: $0 [all|install|start]"; exit 1
    ;;
esac

# ── 6. 立即启动 AP（会断开 wlan0 当前 STA 连接）──
echo "── 立即启动 AP（${AP_IFACE} 当前 STA 连接将被断开）──"
wpa_cli -p /var/run/wpa_supplicant -i ${AP_IFACE} terminate 2>/dev/null
killall hostapd dnsmasq udhcpd 2>/dev/null
sleep 1
ifconfig ${AP_IFACE} ${AP_IP} netmask ${NETMASK} up
dnsmasq --conf-file=/etc/dnsmasq.ap.conf --pid-file=/var/run/dnsmasq.ap.pid &
hostapd -B /etc/hostapd.conf
ip link set ${STA_IFACE} up 2>/dev/null
echo "✅ 热点已启动: ${SSID}"
echo "连上热点后浏览器访问: http://${AP_IP}"
