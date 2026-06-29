#!/bin/bash
set -e

echo "=== OpenRC AP Hotspot & Web Service 一键部署 ==="

# 1. 创建 AP 热点服务脚本
cat > /etc/init.d/ap-hotspot <<'EOF'
#!/sbin/openrc-run
description="Start AP Hotspot and DHCP Server"

depend() {
    need net
    after net
}

start() {
    ebegin "Starting AP Hotspot"
    MAC_SUFFIX=$(cat /sys/class/net/wlan0/address 2>/dev/null | tr -d ':' | tail -c 6)
    RANDOM_ID=$((16#${MAC_SUFFIX} % 9999999 + 1))
    SSID="chenlong-robot-${RANDOM_ID}"

    cat > /etc/hostapd.conf <<EOT
interface=wlan0
driver=nl8011
ssid=${SSID}
hw_mode=g
channel=6
auth_algs=1
EOT

    cat > /etc/udhcpd.conf <<EOT
start 192.168.4.100
end 192.168.4.200
interface wlan0
lease 86400
opt subnet 255.255.255.0
opt router 192.168.4.1
opt dns 192.168.4.1
EOT

    killall wpa_supplicant hostapd udhcpd 2>/dev/null
    sleep 1

    ifconfig wlan0 192.168.4.1 netmask 255.255.255.0 up
    udhcpd /etc/udhcpd.conf &
    hostapd -B /etc/hostapd.conf
    eend $?
}

stop() {
    ebegin "Stopping AP Hotspot"
    killall hostapd udhcpd
    eend $?
}
EOF

# 2. 创建 Web 服务脚本
cat > /etc/init.d/web-service <<'EOF'
#!/sbin/openrc-run
description="Setup wlan1 and Start Web Service"

depend() {
    need net
    after net ap-hotspot
}

start() {
    ebegin "Starting Web Service"
    if ! iw dev | grep -q "wlan1"; then
        echo "Creating wlan1 interface..."
        ip link set wlan0 down
        iw phy phy0 interface add wlan1 type managed
        sleep 2
    fi

    ip link set wlan0 up
    ip link set wlan1 up
    sleep 5

    chmod +x /root/AKA-00/init.sh
    /root/AKA-00/init.sh
    eend $?
}

stop() {
    ebegin "Stopping Web Service"
    eend $?
}
EOF

# 3. 赋予执行权限
chmod +x /etc/init.d/ap-hotspot /etc/init.d/web-service

# 4. 注册到默认运行级别
rc-update add ap-hotspot default
rc-update add web-service default

# 5. 立即启动服务
rc-service ap-hotspot start
rc-service web-service start

echo "✅ 部署完成！"
echo "AP热点已启动，请搜索 SSID: chenlong-robot-xxxxxxx"
echo "Web服务已启动，连接热点后访问: http://192.168.4.1"