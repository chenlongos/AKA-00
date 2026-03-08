#!/bin/sh
echo "=== 安装 AP热点 + DHCP(稳定版) ==="

# 生成基于 MAC 地址的固定 SSID ID (1-99)
MAC_SUFFIX=$(cat /sys/class/net/wlan0/address 2>/dev/null | tr -d ':' | tail -c 2)
RANDOM_ID=$((16#${MAC_SUFFIX} % 99 + 1))
SSID="chenlong-robot-${RANDOM_ID}"

# 热点配置
cat > /etc/hostapd.conf <<EOF
interface=wlan0
driver=nl80211
ssid=${SSID}
hw_mode=g
channel=6
auth_algs=1
EOF

# DHCP配置 (手机必通)
cat > /etc/udhcpd.conf <<'EOF'
start 192.168.4.100
end 192.168.4.200
interface wlan0
lease 86400
opt subnet 255.255.255.0
opt router 192.168.4.1
opt dns 192.168.4.1
EOF

# 自启脚本 (延时启动，不抢系统网络)
cat > /etc/init.d/S98apstart <<'EOF'
#!/bin/sh
sleep 20
killall wpa_supplicant hostapd udhcpd 2>/dev/null

ifconfig wlan0 192.168.4.1 netmask 255.255.255.0 up
udhcpd /etc/udhcpd.conf &
hostapd -B /etc/hostapd.conf
exit 0
EOF

chmod 755 /etc/init.d/S98apstart

# 立即启动
killall wpa_supplicant hostapd udhcpd 2>/dev/null
sleep 1
ifconfig wlan0 192.168.4.1 netmask 255.255.255.0 up
udhcpd /etc/udhcpd.conf &
hostapd -B /etc/hostapd.conf

echo "✅ 热点: ${SSID}"
echo "✅ DHCP已启动"
echo "连接后访问: http://192.168.4.1"
echo "已启用wlan1 作为wifi连接网口"

#----------------------------------------------------------------------------

#!/bin/sh

BASE_DIR="/root/"
LOG_FILE="/tmp/wifi_web.log"

mkdir -p $BASE_DIR

echo "===== WiFi Web Setup Start =====" > $LOG_FILE

############################################
# 1. 创建 wlan1 (STA接口)
############################################

if ! iw dev | grep -q "wlan1"; then
    echo "Creating wlan1..." >> $LOG_FILE
    iw phy phy0 interface add wlan1 type managed
    sleep 1
fi

cat > /etc/init.d/S99webstart <<'EOF'
#!/bin/sh
sleep 30

if ! iw dev | grep -q "wlan1"; then
    echo "Creating wlan1 interface..."
    # 注意：某些驱动要求先关闭 wlan0 才能添加虚拟接口
    ip link set wlan0 down
    iw phy phy0 interface add wlan1 type managed
    sleep 2
fi

ip link set wlan0 up
ip link set wlan1 up

sleep 5
chmod +x /root/AKA-00/init.sh
/root/AKA-00/init.sh
exit 0
EOF

chmod 755 /etc/init.d/S99webstart