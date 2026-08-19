#!/bin/sh
echo "=== 安装 AP热点 + DHCP(稳定版) ==="

# 生成基于 MAC 地址的固定 SSID ID (绝对唯一)
# 使用完整 MAC 的后 6 位，约 1680 万种可能，映射到 1-9999999
MAC_SUFFIX=$(cat /sys/class/net/wlan0/address 2>/dev/null | tr -d ':' | tail -c 6)
RANDOM_ID=$((16#${MAC_SUFFIX} % 9999999 + 1))
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

# 自启脚本 (轮询 wlan0 就绪，代替盲等 sleep 20)
cat > /etc/init.d/S98apstart <<'EOF'
#!/bin/sh
# 等待 wlan0 就绪（轮询，最多 20 秒）
i=0
while [ $i -lt 40 ]; do
    [ -d /sys/class/net/wlan0 ] && break
    sleep 0.5
    i=$((i+1))
done

killall wpa_supplicant hostapd udhcpd 2>/dev/null
sleep 1

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
# 等待 wifi phy 就绪（轮询，最多 30 秒，代替盲等 sleep 30）
i=0
while [ $i -lt 60 ]; do
    iw dev 2>/dev/null | grep -q "phy#0" && break
    sleep 0.5
    i=$((i+1))
done

if ! iw dev | grep -q "wlan1"; then
    echo "Creating wlan1 interface..."
    ip link set wlan0 down
    iw phy phy0 interface add wlan1 type managed
    sleep 1
fi

ip link set wlan0 up
ip link set wlan1 up

# Disable conflicting inittab respawn — init.sh handles its own restart
if grep -q '^acm:.*init\.sh' /etc/inittab 2>/dev/null; then
    sed -i 's/^acm:/# acm:/' /etc/inittab
    kill -HUP 1 2>/dev/null || true
fi

chmod +x /root/AKA-00/init.sh
exec /root/AKA-00/init.sh
EOF

chmod 755 /etc/init.d/S99webstart