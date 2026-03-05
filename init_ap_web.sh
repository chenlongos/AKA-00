#!/bin/sh
echo "=== 安装 AP热点 + DHCP(稳定版) ==="

# 热点配置
cat > /etc/hostapd.conf <<'EOF'
interface=wlan0
driver=nl80211
ssid=sg2002-01
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

echo "✅ 热点: sg2002-01"
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

############################################
# 4. 创建 Web UI
############################################

#cat > $WWW_DIR/index.html <<'EOF'
#<!DOCTYPE html>
#<html>
#<head>
#    <meta charset="utf-8">
#    <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
#    <title>SG2002 WiFi Config</title>
#    <style>
#        body { font-family: -apple-system, sans-serif; background: #f2f2f7; margin: 0; color: #1c1c1e; max-width: 1280px; width:100%; margin-left: 50%; transform: translateX(-50%); }
#        .header { background: #fff; padding: 16px 20px; border-bottom: 0.5px solid #c6c6c8; display: flex; justify-content: space-between; align-items: center; position: sticky; top: 0; z-index: 100; }
#        .header h1 { font-size: 17px; margin: 0; font-weight: 600; }
#        .refresh { color: #007aff; font-size: 15px; cursor: pointer; user-select: none; }
#        .refresh.disabled { color: #8e8e93; cursor: not-allowed; }
#
#        .list { padding: 16px; }
#        .item { background: #fff; border-radius: 10px; margin-bottom: 12px; box-shadow: 0 1px 2px rgba(0,0,0,0.05); overflow: hidden; }
#        .item.connected-item { border: 2px solid #34c759; }
#        .item-row { padding: 14px 16px; display: flex; justify-content: space-between; align-items: center; cursor: pointer; }
#        .ssid-box { display: flex; flex-direction: column; }
#        .ssid-name { font-size: 16px; font-weight: 500; display: flex; align-items: center; gap: 6px; }
#        .ssid-sub { font-size: 12px; color: #8e8e93; margin-top: 2px; }
#
#        .badge { background: #34c759; color: #fff; font-size: 10px; padding: 2px 6px; border-radius: 4px; font-weight: 600; }
#        .sig-box { display: flex; align-items: flex-end; gap: 2px; height: 12px; }
#        .bar { width: 3px; background: #e5e5ea; border-radius: 0.5px; }
#        .bar.active { background: #34c759; }
#
#        .expand-box { display: none; padding: 0 16px 16px; background: #fdfdfd; border-top: 0.5px solid #f2f2f7; }
#        input { width: 100%; padding: 12px; border: 1px solid #d1d1d6; border-radius: 8px; margin: 12px 0; font-size: 16px; box-sizing: border-box; outline: none; }
#        button { width: 100%; padding: 12px; border: none; border-radius: 8px; background: #007aff; color: #fff; font-size: 16px; font-weight: 600; cursor: pointer; }
#        button:active { opacity: 0.8; }
#        button:disabled { background: #aeaeb2; }
#        .status-msg { font-size: 13px; text-align: center; margin-top: 10px; min-height: 1.2em; }
#    </style>
#</head>
#<body>
#
#<div class="header">
#    <div style="display:flex; flex-direction:column;">
#        <h1>无线局域网</h1>
#        <div id="connectionStatus" style="font-size: 12px; color: #8e8e93; margin-top: 2px;">
#            正在获取状态...
#        </div>
#    </div>
#    <div id="refBtn" class="refresh" onclick="loadList()">刷新扫描</div>
#</div>
#
#<div class="list" id="wifiList"></div>
#
#<script>
#let scanning = false;
#let connecting = false;
#
#function getBars(signal) {
#    let level = signal > -60 ? 4 : signal > -70 ? 3 : signal > -80 ? 2 : 1;
#    let h = '';
#    for(let i=1; i<=4; i++) h += `<div class="bar ${i<=level?'active':''}" style="height:${i*25}%"></div>`;
#    return `<div class="sig-box">${h}</div>`;
#}
#
#async function updateStatus() {
#    try {
#        const r = await fetch('/status');
#        const data = await r.json();
#        const statusEl = document.getElementById('connectionStatus');
#
#        if (data.ssid) {
#            statusEl.innerHTML = `<span style="color:#34c759">●</span> 已连接: <b>${data.ssid}</b> | IP: <b style="color:#007aff">${data.ip}</b>`;
#        } else {
#            statusEl.innerHTML = `<span style="color:#ff3b30">○</span> 未连接任何网络`;
#        }
#    } catch (e) {
#        console.error("无法获取状态");
#    }
#}
#
#async function loadList() {
#    if(scanning || connecting) return;
#
#    const btn = document.getElementById('refBtn');
#    scanning = true;
#    btn.innerText = "刷新中...";
#    btn.classList.add('disabled');
#
#    try {
#        const r = await fetch('/scan');
#        const data = await r.json();
#        let html = '';
#        data.list.forEach(w => {
#            html += `
#            <div class="item ${w.is_connected ? 'connected-item' : ''}">
#                <div class="item-row" onclick="toggle('${w.id}')">
#                    <div class="ssid-box">
#                        <div class="ssid-name">
#                            ${w.ssid} ${w.is_connected ? '<span class="badge">已连接</span>' : ''}
#                        </div>
#                        <div class="ssid-sub">${w.secured ? '🔒 安全' : '🔓 公开'} | ${w.signal} dBm</div>
#                    </div>
#                    ${getBars(w.signal)}
#                </div>
#                <div class="expand-box" id="exp-${w.id}">
#                    ${w.secured ? `<input type="password" id="pw-${w.id}" placeholder="输入密码">` : '<div style="font-size:13px;color:#8e8e93;padding:10px 0;">此网络无需密码</div>'}
#                    <button id="btn-${w.id}" onclick="connect('${w.id}', '${encodeURIComponent(w.ssid)}', ${w.secured})">连接网络</button>
#                    <div class="status-msg" id="msg-${w.id}"></div>
#                </div>
#            </div>`;
#        });
#        document.getElementById('wifiList').innerHTML = html || '<div style="text-align:center;padding:40px;color:#8e8e93;">未发现网络</div>';
#        updateStatus(); // 扫描完顺便更新顶部状态
#    } finally {
#        scanning = false;
#        btn.innerText = "刷新扫描";
#        btn.classList.remove('disabled');
#    }
#}
#
#function toggle(id) {
#    document.querySelectorAll('.expand-box').forEach(el => {
#        if(el.id !== 'exp-'+id) el.style.display = 'none';
#    });
#    const target = document.getElementById('exp-'+id);
#    target.style.display = target.style.display === 'block' ? 'none' : 'block';
#}
#
#async function connect(id, ssidEnc, secured) {
#    const ssid = decodeURIComponent(ssidEnc);
#    const pwd = secured ? document.getElementById('pw-'+id).value : "";
#    const msgBox = document.getElementById('msg-'+id);
#    const btn = document.getElementById('btn-'+id);
#
#    if(secured && !pwd) return alert("请输入密码");
#
#    connecting = true;
#    btn.disabled = true;
#    msgBox.innerText = "正在连接并获取地址...";
#    msgBox.style.color = "#8e8e93";
#
#    try {
#        const r = await fetch('/connect', {
#            method: 'POST',
#            body: JSON.stringify({ssid: ssid, password: pwd})
#        });
#        const res = await r.text();
#        const [status, info] = res.split('|');
#        msgBox.innerText = info;
#        msgBox.style.color = status === 'success' ? '#34c759' : '#ff3b30';
#        if(status === 'success') setTimeout(loadList, 2500);
#    } catch(e) {
#        msgBox.innerText = "请求超时";
#    } finally {
#        connecting = false;
#        btn.disabled = false;
#    }
#}
#
#// 页面加载时自动扫描
#loadList();
#</script>
#</body>
#</html>
#EOF
#
#############################################
## 5. 创建 Python 服务
#############################################
#
#cat > $BASE_DIR/wifi_server.py <<'EOF'
#import json, os, time, subprocess, re, base64
#from http.server import BaseHTTPRequestHandler, HTTPServer
#
#INTERFACE = "wlan1"
#CTRL_PATH = "/var/run/wpa_supplicant"
#
#def ensure_wpa_env():
#    """彻底解决内核注册冲突的初始化函数"""
#    if not os.path.exists(CTRL_PATH):
#        os.makedirs(CTRL_PATH, exist_ok=True)
#
#    # 检查控制套接字文件是否存在
#    socket_file = f"{CTRL_PATH}/{INTERFACE}"
#    if not os.path.exists(socket_file):
#        print(f"--- 正在重新初始化 {INTERFACE} 接口 ---")
#        # 1. 杀掉旧进程，防止僵尸实例占用内核 nl80211 句柄
#        os.system("killall -9 wpa_supplicant 2>/dev/null")
#        time.sleep(0.5)
#
#        # 2. 清理残留的无效 socket 文件
#        os.system(f"rm -rf {socket_file}")
#
#        # 3. 强制重置接口状态
#        os.system(f"ip link set {INTERFACE} down")
#        os.system(f"ip link set {INTERFACE} up")
#        time.sleep(0.5)
#
#        # 4. 启动后台进程 (使用 -D nl80211)
#        # 这里的 -C 参数必须保证 CTRL_PATH 是目录
#        cmd = f"wpa_supplicant -D nl80211 -i {INTERFACE} -C {CTRL_PATH} -B"
#        os.system(cmd)
#
#        # 5. 给内核一点反应时间
#        for _ in range(10):
#            if os.path.exists(socket_file):
#                print(f"--- {INTERFACE} 初始化成功 ---")
#                return True
#            time.sleep(0.5)
#        return False
#    return True
#
#def get_current_ip():
#    """快速获取 wlan1 的当前 IP"""
#    ip = subprocess.getoutput(f"ip addr show {INTERFACE} | grep 'inet ' | awk '{{print $2}}' | cut -d/ -f1")
#    return ip if ip else "未分配"
#
#def get_wifi_list():
#    if not ensure_wpa_env():
#        return {"list": [], "error": "WPA_INIT_FAILED"}
#
#    # 触发扫描
#    os.system(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} scan > /dev/null 2>&1")
#    time.sleep(1.5) # 增加扫描等待时间
#    raw_results = subprocess.getoutput(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} scan_results")
#
#    # 获取状态
#    current_status = subprocess.getoutput(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} status")
#    connected_ssid = None
#    if "wpa_state=COMPLETED" in current_status:
#        ssid_match = re.search(r"^ssid=(.*)$", current_status, re.MULTILINE)
#        if ssid_match: connected_ssid = ssid_match.group(1)
#
#    unique_wifi = {}
#    lines = raw_results.split('\n')
#    for line in lines[1:]:
#        parts = line.split('\t')
#        if len(parts) >= 5:
#            ssid = parts[4].strip()
#            if not ssid: continue
#            signal = int(parts[2])
#            safe_id = base64.b64encode(ssid.encode()).decode().replace('=', '')
#            if ssid not in unique_wifi or signal > unique_wifi[ssid]['signal']:
#                unique_wifi[ssid] = {
#                    "ssid": ssid, "id": safe_id, "signal": signal,
#                    "secured": not (parts[3] == "[ESS]" or parts[3] == "[WPS][ESS]"),
#                    "is_connected": (ssid == connected_ssid)
#                }
#
#    return {"list": sorted(unique_wifi.values(), key=lambda x: (not x['is_connected'], -x['signal'])), "connected": connected_ssid}
#
#def do_connect(ssid, password):
#    ensure_wpa_env()
#    # 交互式连接，不重启进程，防止 kernel 再次报错
#    os.system(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} remove_network all > /dev/null")
#    net_id = subprocess.getoutput(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} add_network").strip().split('\n')[-1]
#
#    os.system(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} set_network {net_id} ssid '\"{ssid}\"'")
#    if password:
#        os.system(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} set_network {net_id} psk '\"{password}\"'")
#    else:
#        os.system(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} set_network {net_id} key_mgmt NONE")
#
#    os.system(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} select_network {net_id}")
#
#    for _ in range(15):
#        status = subprocess.getoutput(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} status")
#        if "wpa_state=COMPLETED" in status:
#            os.system(f"udhcpc -i {INTERFACE} -n -q -T 5")
#            ip = subprocess.getoutput(f"ip addr show {INTERFACE} | grep 'inet ' | awk '{{print $2}}' | cut -d/ -f1")
#            return f"success|连接成功! IP: {ip}"
#        time.sleep(1)
#    return "error|连接超时"
#
#class Handler(BaseHTTPRequestHandler):
#    def do_GET(self):
#        if self.path == "/":
#            self.send_response(200); self.send_header("Content-type", "text/html; charset=utf-8"); self.end_headers()
#            with open("/root/wifi_web/www/index.html", "rb") as f: self.wfile.write(f.read())
#        elif self.path == "/status":
#            self.send_response(200); self.send_header("Content-type", "application/json"); self.end_headers()
#            # 获取当前连接的 SSID
#            status_raw = subprocess.getoutput(f"wpa_cli -p {CTRL_PATH} -i {INTERFACE} status")
#            ssid_match = re.search(r"^ssid=(.*)$", status_raw, re.MULTILINE)
#            current_ssid = ssid_match.group(1) if ssid_match else None
#
#            data = {
#                "ssid": current_ssid,
#                "ip": get_current_ip() if current_ssid else "N/A"
#            }
#            self.wfile.write(json.dumps(data).encode())
#        elif self.path == "/scan":
#            self.send_response(200); self.send_header("Content-type", "application/json"); self.end_headers()
#            self.wfile.write(json.dumps(get_wifi_list()).encode())
#    def do_POST(self):
#        if self.path == "/connect":
#            data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
#            res = do_connect(data['ssid'], data.get('password', ''))
#            self.send_response(200); self.end_headers(); self.wfile.write(res.encode())
#
#if __name__ == "__main__":
#    os.system(f"ip link set {INTERFACE} up")
#    HTTPServer(('', 80), Handler).serve_forever()
#EOF

# 自启脚本 (延时启动，不抢系统网络)
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