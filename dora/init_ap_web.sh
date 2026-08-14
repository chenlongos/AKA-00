#!/bin/sh
# =============================================================================
# init_ap_web.sh —— 安装器（重写版）
#
# 在板子上执行一次：写 AP 热点配置 + 写开机自启脚本，重启后自动拉起
# [wlan0=AP 热点] + [wlan1=STA 上联] + [dora init.sh]。
#
# 相对旧版的关键改动：
#
#   1. 删掉盲等 sleep 20/30/2/5 —— 全部改成「条件轮询 + 超时上限」：
#        - AP 就绪   -> 重试式拉起 hostapd；就绪判定用 pidof + 192.168.4.1 地址
#        - wlan1 上联 -> 轮询 `wpa_cli -i wlan1 status` 到 state=COMPLETED
#        - DHCP 拿到  -> 轮询默认路由出现
#        - dora      -> reuse init.sh 自带的 60s curl 端口探测，不再额外 sleep
#      机器快就快、慢也封顶，不会无限挂。
#
#   2. 修正 S98/S99 竞态：旧版 S98 建一次 wlan1、S99 又「wlan0 down 后再建一次」
#      且两处都 killall 互相打架。合并成单一 /etc/init.d/S98apstart 顺序执行，
#      并删除遗留的 S99webstart，杜绝重复建接口 / 重复跑 init.sh。
#
#   3. 角色顺序（符合单射频 cfg80211 实测约束，phy0: managed<=1 且 AP<=1）：
#      必须【先把 wlan0 切成 AP】 -> 再【建 wlan1 作 managed】= 1AP+1managed 合法。
#      若在 wlan0 仍为 managed 时去建 wlan1 managed 会因 2 个 managed 直接失败。
#
#   4. 安装时默认【不】立即切热点 —— 在 live 板上 wlan0 此刻正承担 SSH 上联，
#      就地切 AP 会掉线。装完「重启」生效。
#      想立刻临时起 AP 测试：sh init_ap_web.sh --now （慎用，会断当前网络）。
#
#   遗留约束说明：phy0 是单射频，AP(ch6) 与 STA 会共享同一天线收发。hostapd 固定
#   channel=6 是为了和 wlan1 所连网络的信道尽量一致，避免驱动频繁跳信道导致 AP 抖动。
# =============================================================================

# 固定 SSID：基于 wlan0 MAC 尾号，映射 1-9999999（绝对唯一）
MAC_SUFFIX=$(cat /sys/class/net/wlan0/address 2>/dev/null | tr -d ':' | tail -c 6)
MAC_SUFFIX="${MAC_SUFFIX:-0}"
RANDOM_ID=$((16#${MAC_SUFFIX} % 9999999 + 1))
SSID="chenlong-robot-${RANDOM_ID}"

# --- 1. 写 hostapd.conf（wlan0 = AP，开放热点 ch6）---
cat > /etc/hostapd.conf <<EOF
interface=wlan0
driver=nl80211
ssid=${SSID}
hw_mode=g
channel=6
auth_algs=1
EOF

# --- 2. 写 udhcpd.conf（AP 网段 192.168.4.x，手机必通）---
cat > /etc/udhcpd.conf <<'EOF'
start 192.168.4.100
end   192.168.4.200
interface wlan0
lease 86400
opt subnet 255.255.255.0
opt router 192.168.4.1
opt dns   192.168.4.1
EOF

# --- 3. 写开机自启脚本 /etc/init.d/S98apstart（合并原 S98+S99，单点顺序执行）---
cat > /etc/init.d/S98apstart <<'APEOF'
#!/bin/sh
#
# dora 板端启动 —— 单脚本完成: [wlan0=AP 热点] + [wlan1=STA 上联] + [拉起 dora init.sh]
# 所有等待均为「条件轮询 + 超时上限」，不是盲等。日志: /tmp/boot-ap.log
#
LOG=/tmp/boot-ap.log
: > "$LOG"
say(){ echo "$*"; echo "[$(date '+%T')] $*" >> "$LOG"; }

AP_SSID=$(grep '^ssid=' /etc/hostapd.conf 2>/dev/null | head -1 | cut -d= -f2)

# ============================================================================
# 一、把 wlan0 切成 AP（先切 AP，这是 1AP+1managed 的 合法 顺序前提）
#     若与系统自动起的 wlan0 STA 抢占冲突，则清掉对手、退避重试，上限 25s。
# ============================================================================
say "== [$(basename "$0")] bring up AP on wlan0 =="
killall wpa_supplicant hostapd udhcpd 2>/dev/null
sleep 1                      # 唯一的短等待：让旧 wlan0 managed 状态释放

AP_OK=no
AP_DEADLINE=$(( $(date +%s) + 25 ))
while [ "$(date +%s)" -lt "$AP_DEADLINE" ]; do
    ifconfig wlan0 192.168.4.1 netmask 255.255.255.0 up 2>>"$LOG"
    hostapd -B /etc/hostapd.conf 2>>"$LOG"
    sleep 2
    if pidof hostapd >/dev/null 2>&1; then
        sleep 1                                   # 再确认一次，防被系统抢占后杀死
        if pidof hostapd >/dev/null 2>&1; then AP_OK=yes; break; fi
    else
        say "  hostapd 未驻留，疑似与 wlan0 抢占冲突，清理后重试..."
        killall wpa_supplicant udhcpd 2>/dev/null  # 只清对手，不动 hostapd/AP
    fi
done

if [ "$AP_OK" = yes ]; then
    udhcpd /etc/udhcpd.conf 2>>"$LOG" &
    say "AP ready: ${AP_SSID:-?} @ 192.168.4.1"
else
    say "WARN: AP 未就绪 (25s 超时)，继续后续步骤"
    udhcpd /etc/udhcpd.conf 2>>"$LOG" &
fi

# ============================================================================
# 二、wlan1 作 STA 上联（此刻 wlan0 已是 AP，1managed 空位可加 wlan1）
#     读 /etc/wpa_supplicant.conf（含校园网/客户网凭据）
# ============================================================================
if ! iw dev 2>/dev/null | grep -q "wlan1"; then
    say "creating wlan1 (managed) on phy0 ..."
    iw phy phy0 interface add wlan1 type managed 2>>"$LOG" \
      || say "WARN: 创建 wlan1 失败（驱动/接口组合不支持？）"
fi

if iw dev 2>/dev/null | grep -q "wlan1"; then
    ip link set wlan1 up 2>>"$LOG"
    # 仅当已有 wpa_supplicant 时才加；不要在它有多个时重复 -B
    wpa_supplicant -B -i wlan1 -c /etc/wpa_supplicant.conf 2>>"$LOG" || true

    # 等 wlan1 关联成功（state=COMPLETED），上限 45s —— 替代旧 30s 盲等
    STA_DEADLINE=$(( $(date +%s) + 45 ))
    sleep 1
    while [ "$(date +%s)" -lt "$STA_DEADLINE" ]; do
        # 兼容新旧 wpa_supplicant: 旧版输出 wpa_state=COMPLETED, 新版输出 state=COMPLETED
        if wpa_cli -i wlan1 status 2>/dev/null | grep -Eq '^(state|wpa_state)=COMPLETED'; then break; fi
        sleep 0.5
    done
    STA_STATE=$(wpa_cli -i wlan1 status 2>/dev/null | grep -E '^(state|wpa_state)=' | head -1)
    say "wlan1 associate: ${STA_STATE:-unknown}"

    # DHCP 拿 IP（后台 + 轮询默认路由），上限 20s
    udhcpc -i wlan1 2>>"$LOG" &
    ROUTE_DEADLINE=$(( $(date +%s) + 20 ))
    while [ "$(date +%s)" -lt "$ROUTE_DEADLINE" ]; do
        if ip route 2>/dev/null | grep -q default; then break; fi
        sleep 0.5
    done
    if ip route 2>/dev/null | grep -q default; then
        W1IP=$(ip -4 addr show wlan1 2>/dev/null | awk '/inet /{print $2; exit}')
        say "uplink OK, default route up; wlan1=${W1IP:-?}"
    else
        say "WARN: wlan1 上联未拿到默认路由 (20s)，AP 仍可用"
    fi
fi

# ============================================================================
# 三、拉起 dora（init.sh 自带 60s curl 端口探测，无需这里额外 sleep）
# ============================================================================
if [ -x /root/AKA-00/init.sh ]; then
    say "launching /root/AKA-00/init.sh"
    /root/AKA-00/init.sh >>"$LOG" 2>&1 &
else
    say "WARN: /root/AKA-00/init.sh 不存在，跳过 dora"
fi

exit 0
APEOF
chmod 755 /etc/init.d/S98apstart

# --- 4. 删除遗留的 S99webstart，避免并发重复建 wlan1 / 重复跑 init.sh ---
rm -f /etc/init.d/S99webstart

# --- 5. 可选：立即临时起 AP（会断开当前 SSH/网络）---
if [ "$1" = "--now" ]; then
    echo ">>> 立即临时启用 AP (${SSID})，可能断开当前网络连接..."
    killall wpa_supplicant hostapd udhcpd 2>/dev/null
    sleep 1
    ifconfig wlan0 192.168.4.1 netmask 255.255.255.0 up
    udhcpd /etc/udhcpd.conf &
    hostapd -B /etc/hostapd.conf
    echo ">>> 临时热点已启用: ${SSID} (192.168.4.1)  ssh 连接会断"
fi

echo "======================================================"
echo "完成。安装时未切换热点（避免断开当前 SSH）。"
echo "  重启后自动生效: AP=${SSID}  +  wlan1 上联  +  dora init.sh"
echo "  开机日志:       /tmp/boot-ap.log"
echo "  立即临时启用:   sh $0 --now   (注意会断开当前网络)"
echo "======================================================"