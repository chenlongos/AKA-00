#!/bin/sh
# =============================================================================
# AKA-00 capp 启动脚本（SG2002 板子）
#
# 自愈循环：capp 崩溃自动重启。OTA 进行中（$AKA_HOME/.ota/aka-ota-lock 存在）时等待。
#
# 用法:
#   AKA_HOME=$HOME/AKA-00 ./init.sh
# =============================================================================

# 调试用：把 init.sh 自身和它拉起的 capp 的 stdout/stderr 落到 /root/aka-boot.log。
# /tmp 是 tmpfs 存不住，root 分区每次开机覆盖写。排查完可以删掉这一行。
exec > /root/aka-boot.log 2>&1

AKA_HOME="${AKA_HOME:-$HOME/AKA-00}"
BIN="$AKA_HOME/aka-capp"
# OTA 锁。**原来在 /tmp** —— 那时是 tmpfs（内存盘），重启自动清掉，所以"陈旧锁"
# 不可能存在。现在整条 OTA 链路的暂存都从 /tmp 挪到了 $AKA_HOME/.ota（板上 /tmp 只有
# 53MB 内存盘，20MB 的固件放那儿实测把内存打爆过），锁也跟着挪了 —— 于是**必须自己
# 处理陈旧锁**：磁盘上的文件跨得过重启，而这个循环是纯存在性检查、没有超时逃逸，
# 一个陈旧的锁会让 capp 永远起不来。下面启动时清一次。
LOCK_FILE="$AKA_HOME/.ota/aka-ota-lock"
PID_FILE="/var/run/aka-capp.pid"

# 防止重复实例：**必须查 init.sh 自己的 pid，不能查 capp 的**。
# 原来查 /var/run/aka-capp.pid（capp 的 pid）：只要在"capp 刚好没在跑"的瞬间起第二个
# init.sh（例如部署时先 kill capp 再起脚本），检查就通不过拦截 —— 于是两个守护循环并存，
# 各自拉起一个 capp，第二个抢不到 :80 → "bind failed → 退出 → 2 秒后重启"的循环
# （实测踩到两次）。查自己的 pid 就与 capp 在不在跑无关了。
INIT_PID_FILE="/var/run/aka-init.pid"
if [ -f "$INIT_PID_FILE" ]; then
    _old=$(cat "$INIT_PID_FILE" 2>/dev/null)
    if [ -n "$_old" ] && kill -0 "$_old" 2>/dev/null; then
        echo "[init] 已有 init.sh 在跑 (pid $_old)，退出"
        exit 0
    fi
fi
echo $$ > "$INIT_PID_FILE"

# 清陈旧 OTA 锁（理由见上面 LOCK_FILE 处）。为什么这里清是安全的：
# 锁只对"正在进行的那一次 OTA"有意义，跨不过一次重启；而上面刚做过单实例保护，
# 所以能走到这行说明要么是开机、要么是 OTA 自己 exec 过来的 —— 后者那时安装器
# 已经把锁删了。两种情况下那个残留的锁都是死锁，必须清。
rm -f "$LOCK_FILE" 2>/dev/null || true

if [ ! -x "$BIN" ]; then
    # 兜底：scp/tar/zip 传输可能丢可执行位
    chmod +x "$BIN" 2>/dev/null || true
fi
if [ ! -x "$BIN" ]; then
    echo "[init] $BIN not found or not executable"
    exit 1
fi

# config.toml 必须是可用文件：空文件 = 所有配置项静默取默认值。
# 踩过：一次 OTA 换包中途断电把 config.toml 写成 0 字节，车于是按默认配置跑
# （当时 motor 默认还是 dev → 整台车是 mock，车不动，界面也没提示）。
if [ ! -s "$AKA_HOME/config.toml" ]; then
    echo "[init] !!! $AKA_HOME/config.toml 不存在或为空 —— 所有配置项会取默认值，先查这个" >&2
fi

# 摄像头分辨率环境（可选，config.toml 也控制）
CAM_WIDTH=$(grep -E '^\s*width\s*=' "$AKA_HOME/config.toml" 2>/dev/null | grep -oE '[0-9]+' | head -1)
CAM_HEIGHT=$(grep -E '^\s*height\s*=' "$AKA_HOME/config.toml" 2>/dev/null | grep -oE '[0-9]+' | head -1)
[ -n "$CAM_WIDTH" ] && export CAMERA_WIDTH="$CAM_WIDTH"
[ -n "$CAM_HEIGHT" ] && export CAMERA_HEIGHT="$CAM_HEIGHT"

# 显示引擎（可选，有屏才需要）
if [ -w /sys/class/graphics/fb0/state ]; then
    echo 1 > /sys/class/graphics/fb0/state 2>/dev/null || true
    echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null || true
fi

# ─────────────────────────────────────────────────────────────────────────────
# AP 热点：按 MAC 分信道 + 11n/WMM + 200ms beacon（课堂多车场景）
#
# 出厂配置全部是 channel=6、802.11g legacy、beacon 100ms、无 WMM —— 30 台车同房间
# 时单靠 beacon 就吃掉大半空口时间，数据帧再排队 → 找不到热点/摇杆延迟/开摄像头卡死。
#
# /etc/hostapd.conf 由 init_ap_web.sh **一次性**生成（S97akaap 靠锁只跑一次），
# 老车不会跟着 OTA 更新 —— 所以落点放在 init.sh（随 OTA 升级），每次开机幂等对齐；
# 配置没变就不重启 hostapd（不打断正在使用的热点）。
#
# 信道 = wlan0 MAC 最后一位十六进制 % 3 → 1/6/11（30 台车大致 1:1:1 分散到
# 三个互不重叠的信道；同一信道内的车仍然共享空口，但至少不是全员挤在 6）。
# ─────────────────────────────────────────────────────────────────────────────
HOSTAPD_CONF=/etc/hostapd.conf

ap_channel_for_mac() {
    _mac=$(cat /sys/class/net/wlan0/address 2>/dev/null | tr -d ':')
    _last=$(printf '%s' "$_mac" | tail -c 1)
    case "$_last" in
        ""|*[!0-9a-fA-F]*) echo 6 ;;   # MAC 读不到：用中间信道
        *)
            case $((16#${_last} % 3)) in
                0) echo 1 ;;
                1) echo 6 ;;
                *) echo 11 ;;
            esac
            ;;
    esac
}

hostapd_conf_sync() {
    _cf="$HOSTAPD_CONF"
    [ -f "$_cf" ] || { echo "[init] 缺 $_cf，跳过 AP 信道同步"; return 0; }
    _want_ch=$(ap_channel_for_mac)
    _new="$_cf.new.$$"
    cp -f "$_cf" "$_new" 2>/dev/null || return 0
    _set_key() {
        if grep -qE "^[[:space:]]*$1=" "$_new"; then
            sed -i "s|^[[:space:]]*$1=.*|$1=$2|" "$_new"
        else
            printf '%s=%s\n' "$1" "$2" >> "$_new"
        fi
    }
    _set_key channel "$_want_ch"
    _set_key ieee80211n 1
    _set_key wmm_enabled 1
    _set_key beacon_int 200
    _set_key ctrl_interface /var/run/hostapd
    # ht_capab 不显式写：多写驱动不认的位会让 hostapd 直接起不来，
    # 交给 hostapd 按 phy 能力自动协商（实测 ht_op_mode=0x11，HT20 生效）。
    if cmp -s "$_cf" "$_new"; then
        rm -f "$_new"
        echo "[init] AP 配置已是最新（channel=$_want_ch）"
        return 0
    fi
    mv -f "$_new" "$_cf"
    echo "[init] AP 配置更新（channel=$_want_ch, 11n, WMM, beacon 200）"
    if ps w 2>/dev/null | grep -q '[h]ostapd'; then
        killall hostapd 2>/dev/null || true
        sleep 1
        mkdir -p /var/run/hostapd 2>/dev/null || true
        if hostapd -B "$_cf" >/dev/null 2>&1; then
            echo "[init] hostapd 已按新配置重启"
        else
            echo "[init] !!! hostapd 启动失败（channel=$_want_ch），热点可能不可用" >&2
        fi
    else
        echo "[init] hostapd 未运行，配置已写入，下次启动生效"
    fi
}

hostapd_conf_sync

# ─────────────────────────────────────────────────────────────────────────────
# 课堂画质档（一次性迁移）：降低浏览器取流带宽 + 提升流畅度
#
# 30 台同房间时，每台浏览器看摄像头是 2~4Mbps，2.4G 空口会被打满。实测本板
# （单核 C906，取流帧率主要由像素数决定；quality 几乎不影响帧率）：
#   直通 640x360      : ~4.2Mbps @ 14fps（有人看流时暂停屏显）
#   缩放 320x180 q75  : ~0.89Mbps @ 16.1fps
#   缩放 288x162 q80  : ~1.07Mbps @ 20.6fps
#   缩放 256x144 q88  : ~1.25Mbps @ 23.1fps（解码降到 160x90，画面偏软）
# 默认 288x162 q80（带一点画质余量换 +28% 帧率），并把 [display] fps_streaming
# 设 0（有人看流时暂停板载屏，把单核让给取流；屏停在最后一帧，浏览器断开后自动恢复）。
# 采集 fps 15→30：取流"有新帧才推"，采集慢会直接卡住出流帧率。
# 检测（抓球 demo）走原生 640 帧，**不受影响**。
#
# config.toml 在 OTA 的 KEEP_FILES 里，推包不会覆盖 —— 老车只能在这里一次性迁移。
# 标记放 /etc（OTA 不换 /etc）：老师之后手动改的配置不会被再次覆盖。
# ─────────────────────────────────────────────────────────────────────────────
CAMERA_PROFILE_MARK=/etc/aka-camera-profile-v2

camera_profile_sync() {
    _cfg="$AKA_HOME/config.toml"
    [ -f "$_cfg" ] || return 0
    [ -f "$CAMERA_PROFILE_MARK" ] && return 0
    # 兼容三档老值：[camera] fps 15（display 的 fps 是 8，行首精确匹配不会误伤）、
    # 分辨率 320x180（出厂）或 400x225（v1 迁移过）、quality 75。
    sed -i \
        -e 's/^stream_scale = false$/stream_scale = true/' \
        -e 's/^stream_width = 320$/stream_width = 288/' \
        -e 's/^stream_height = 180$/stream_height = 162/' \
        -e 's/^stream_width = 400$/stream_width = 288/' \
        -e 's/^stream_height = 225$/stream_height = 162/' \
        -e 's/^stream_quality = 75$/stream_quality = 80/' \
        -e 's/^fps_streaming = 3$/fps_streaming = 0/' \
        -e 's/^fps = 15$/fps = 30/' \
        "$_cfg" 2>/dev/null || return 0
    if grep -q '^stream_scale = true$' "$_cfg"; then
        touch "$CAMERA_PROFILE_MARK"
        echo "[init] 课堂画质档已应用（浏览器流 288x162 q80 @采集30fps，有人看流时暂停屏显）"
    else
        echo "[init] 课堂画质档未应用（config.toml 格式不符，跳过）"
    fi
}
camera_profile_sync

# 关键：export AKA_HOME，否则 capp 的 static/config 路径退化为 cwd 相对路径，
# 从别的目录启动就找不到 static/ 和 config.toml。

export AKA_HOME="$AKA_HOME"

# HTTPS 自签名证书：capp 只读取 cert.pem/key.pem，缺一就重生成（一次性）。
# 注意路径是 $AKA_HOME 不是 $APP_DIR —— 这个脚本里从来没有 APP_DIR 这个变量
# （Python 版 init 的遗留），写成 APP_DIR 时这行恒假，于是 https_init.sh **一次都没跑过**，
# 证书永远不生成，capp 只能退化成"TLS listener disabled"，HTTPS/wss 全不可用。
if [ -x "$AKA_HOME/https_init.sh" ]; then
    "$AKA_HOME/https_init.sh" || echo "[init] https_init.sh failed (will retry next boot)"
else
    echo "[init] 缺 $AKA_HOME/https_init.sh，HTTPS 证书无法自动生成"
fi

# ─────────────────────────────────────────────────────────────────────────────
# WiFi STA：上次连接持久化 + 开机自动重连
#
# 原来 wlan1 的 wpa_supplicant 由 capp 首次使用时自举，**不带 `-c`**（没有配置文件），
# 所以 wpa_supplicant 自己没法把用户连过的网络存下来；而 capp 的
# /etc/aka-wifi.json 只在"8 秒内连上"时才写回 —— 热点稍微慢一点就写不进去，
# 于是下次开机还去连很旧的网络（Tsinghua-Dongsheng / HSU 之类）。
#
# 这里改成：
#   1) 由 init.sh 带 `-c /etc/wpa_supplicant.conf` 起 wpa_supplicant
#      （文件里保证有 ctrl_interface + update_config=1），开机就用上次网络自动关联；
#   2) 后台盯 wpa_state，一旦 COMPLETED 就 save_config，把当前网络（含密码）
#      写回配置文件，下次开机自然重连；
#   3) 同一时刻把它同步进 capp 的 aka-wifi.json —— capp 的自动重连会先
#      remove_network all 再下发凭据，不同步的话它会拿旧凭据把刚连上的网络顶掉。
#
# capp 的 ensure_wpa_env() 只检查控制 socket 在不在，发现我们已起过实例就复用，
# 不会重复拉起；两边控制接口都是 /var/run/wpa_supplicant/wlan1。
# ─────────────────────────────────────────────────────────────────────────────
WPA_CONF=/etc/wpa_supplicant.conf
WPA_CTRL=/var/run/wpa_supplicant
WPA_IF=wlan1
WPA_JSON=/etc/aka-wifi.json

wpa_alive() {
    wpa_cli -p "$WPA_CTRL" -i "$WPA_IF" ping 2>/dev/null | grep -q PONG
}

wpa_state_now() {
    wpa_cli -p "$WPA_CTRL" -i "$WPA_IF" status 2>/dev/null |
        sed -n 's/^wpa_state=//p' | head -1
}

wpa_ssid_now() {
    wpa_cli -p "$WPA_CTRL" -i "$WPA_IF" status 2>/dev/null |
        sed -n 's/^ssid=//p' | head -1
}

# 从 $WPA_CONF 取某个网络字段（ssid / psk）。$2 为空 = 取文件里第一个网络。
wpa_conf_field() {
    awk -v f="$1" -v want="$2" '
        /^[ \t]*network=\{/ { s=""; p=""; inb=1; next }
        /^\}/ {
            if (inb && (want == "" || s == want)) {
                print (f == "ssid" ? s : p); exit
            }
            inb=0; next
        }
        inb && /^[ \t]*ssid="/ { s=$0; sub(/^[^"]*"/, "", s); sub(/".*$/, "", s) }
        inb && /^[ \t]*psk="/  { p=$0; sub(/^[^"]*"/, "", p); sub(/".*$/, "", p) }
    ' "$WPA_CONF"
}

# 把配置文件里的网络同步到 capp 的 aka-wifi.json（$1=指定 ssid，空=第一个网络）
#
# **不能用 mtime 判断新旧**：本板没有 RTC，每次开机时钟从 1970 重新走，文件 mtime
# 实际是"那一次开机的开机秒数"，跨重启完全不可比。踩过：conf 的 mtime(2236s) 恰好
# 大于 json 的 mtime(341s)，两个反方向的迁移全被永久跳过，车永远去连出厂网络。
#
# 改用内容判断：
#   - $1 非空（watchdog 看到当前已连接的网络）：以它为准，强制写回；
#   - $1 为空（开机播种）：JSON 里已经有 ssid 就保持不动（它才是"最后一次成功
#     连接"），只有 JSON 缺失/为空时才用 conf 的第一个网络播种。
sync_aka_wifi_json() {
    [ -f "$WPA_CONF" ] || return 0
    if [ -z "$1" ]; then
        _js=$(sed -n 's/.*"ssid":"\([^"]*\)".*/\1/p' "$WPA_JSON" 2>/dev/null)
        [ -n "$_js" ] && return 0
    fi
    if [ -n "$1" ]; then
        _ssid=$(wpa_conf_field ssid "$1")
        [ -n "$_ssid" ] || return 0     # 指定网络不在文件里：宁可不动 JSON
        _psk=$(wpa_conf_field psk "$_ssid")
    else
        _ssid=$(wpa_conf_field ssid "")
        [ -n "$_ssid" ] || return 0
        _psk=$(wpa_conf_field psk "")
    fi
    case "$_ssid$_psk" in
        *\"*|*\\*)
            # JSON 转义在 shell 里太脆，这种情况只靠 wpa_supplicant.conf 重连
            return 0 ;;
    esac
    if printf '{"ssid":"%s","password":"%s"}\n' "$_ssid" "$_psk" > "$WPA_JSON.tmp"; then
        chmod 600 "$WPA_JSON.tmp" 2>/dev/null
        mv -f "$WPA_JSON.tmp" "$WPA_JSON" && echo "[init] aka-wifi.json <- $_ssid"
    else
        rm -f "$WPA_JSON.tmp"
    fi
}

# 反方向：把 JSON（最后一次成功连接）落到配置文件 —— 内容不一致时才重建。
# **不能按 mtime 判断谁更新**（无 RTC，见 sync_aka_wifi_json 上方的注释）；
# 下面已有内容级判断：目标网络已在 conf 里且 psk 一致就什么都不做。
#
# 为什么需要：capp 的 POST /api/wifi/connect 只在"8 秒内连上"时才把凭据写进
# /etc/aka-wifi.json（慢热点永远写不进去）；而 wpa_supplicant 可能一直是不带 -c 起的那种
# 实例（save_config 空转），于是 /etc/wpa_supplicant.conf 里还是出厂网络（HSU/00000000）。
# 两者相比，JSON 才是"用户最后一次成功连接的记录"，开机必须让它说了算 —— 否则厂网只要
# 在信号范围里就会先连上去，用户以为"又回到初始化的 tinghuaxx 了"。
#
# 把 JSON 里的网络落到配置文件后，wpa_supplicant -c 起来就直接按这个网络关联；
# 后面 watchdog 的 save_config 才有了真正的落盘目标。
# 只在两边内容确实不一致时重写，不丢现有其它 network 块的情况：
# 目标网络已在文件里且 psk 相同 → 不动。
sync_conf_from_json() {
    [ -f "$WPA_JSON" ] || return 0
    _j=$(cat "$WPA_JSON" 2>/dev/null) || return 0
    case "$_j" in
        *\\*) return 0 ;;   # JSON 里的转义在 shell 里解析不可靠，跳过
    esac
    _s=$(printf '%s\n' "$_j" | sed -n 's/.*"ssid":"\([^"]*\)".*/\1/p')
    _p=$(printf '%s\n' "$_j" | sed -n 's/.*"password":"\([^"]*\)".*/\1/p')
    [ -n "$_s" ] || return 0
    if [ -f "$WPA_CONF" ] &&
       [ "$(wpa_conf_field ssid "$_s")" = "$_s" ] &&
       [ "$(wpa_conf_field psk "$_s")" = "$_p" ]; then
        return 0
    fi
    {
        printf 'ctrl_interface=%s\nupdate_config=1\nap_scan=1\n\n' "$WPA_CTRL"
        printf 'network={\n\tssid="%s"\n' "$_s"
        if [ -n "$_p" ]; then
            printf '\tpsk="%s"\n\tkey_mgmt=WPA-PSK\n' "$_p"
        else
            printf '\tkey_mgmt=NONE\n'
        fi
        printf '}\n'
    } > "$WPA_CONF.tmp" &&
        mv -f "$WPA_CONF.tmp" "$WPA_CONF" &&
        echo "[init] wpa_supplicant.conf <- $_s (来自 aka-wifi.json)"
}

# ── 开机自动连接：最多尝试 STA_CONNECT_TIMEOUT 秒（默认 120）──────────────
# 课堂场景（一屋子 30 台车）不需要逐个管理开关：开机照常按上次保存的 WiFi
# 自动扫描/关联，但从启动算起 2 分钟内没连上就**放弃自动重连** —— 停掉 wlan1
# 的 wpa_supplicant 并把接口 down 掉，不再周期扫描（STA 扫描/关联和 AP 共用
# 一副射频，30 台车一起扫会把空口占满：找热点难、摇杆卡）。
#
# 放弃后"手动连接"不受影响：控制台 WiFi 页的 scan/connect 会调用 capp 的
# ensure_wpa_env() 重新拉起 wpa_supplicant —— 想连就再点一次。
# 下次开机会重新给满 2 分钟（上次保存的网络仍是优先目标）。
# 想改时长：启动 init.sh 时设 STA_CONNECT_TIMEOUT（秒，默认 120）。
STA_CONNECT_TIMEOUT="${STA_CONNECT_TIMEOUT:-120}"

# 启动前先把 JSON（最后一次成功连接）落到配置文件，再起 wpa_supplicant。
sync_conf_from_json

# 配置文件兜底：保证 ctrl_interface / update_config=1（save_config 的前提）
if [ ! -f "$WPA_CONF" ]; then
    printf 'ctrl_interface=%s\nupdate_config=1\n' "$WPA_CTRL" > "$WPA_CONF"
else
    grep -q '^[[:space:]]*ctrl_interface=' "$WPA_CONF" 2>/dev/null ||
        printf 'ctrl_interface=%s\n' "$WPA_CTRL" >> "$WPA_CONF"
    grep -q '^[[:space:]]*update_config=1' "$WPA_CONF" 2>/dev/null ||
        printf 'update_config=1\n' >> "$WPA_CONF"
fi

# 起 wpa_supplicant（带 -c）。起不来就退回老方式（不带 -c）—— 至少 WiFi 还能用。
if ! wpa_alive; then
    ip link set "$WPA_IF" up 2>/dev/null
    wpa_supplicant -D nl80211 -i "$WPA_IF" -c "$WPA_CONF" -C "$WPA_CTRL" -B >/dev/null 2>&1
    _i=0
    while [ "$_i" -lt 10 ] && ! wpa_alive; do _i=$((_i + 1)); sleep 0.5; done
    if ! wpa_alive; then
        echo "[init] wpa_supplicant -c 启动失败，退回无配置方式（WiFi 可用但不持久化）"
        wpa_supplicant -D nl80211 -i "$WPA_IF" -C "$WPA_CTRL" -B >/dev/null 2>&1
        _i=0
        while [ "$_i" -lt 10 ] && ! wpa_alive; do _i=$((_i + 1)); sleep 0.5; done
    fi
fi
if wpa_alive; then
    echo "[init] wpa_supplicant ready (conf=$WPA_CONF)"
else
    echo "[init] wpa_supplicant 未就绪"
fi

# 先同步一次，再让 capp 起来（capp 的自动重连会用这个文件）
sync_aka_wifi_json ""

# 后台守护 1：连上就保存当前网络（换网后也会再保存）；
# 超过 STA_CONNECT_TIMEOUT 还没连上 → 放弃自动重连（只保留保存职责，
# 不再重启 wpa_supplicant；手动连接由 WiFi 页按需拉起）。
(
    _saved=""
    _fail=0
    _deadline=$(( $(date +%s) + STA_CONNECT_TIMEOUT ))
    _gave_up=0
    while true; do
        if ! wpa_alive; then
            if [ "$_gave_up" = 0 ]; then
                wpa_supplicant -D nl80211 -i "$WPA_IF" -c "$WPA_CONF" -C "$WPA_CTRL" -B \
                    >/dev/null 2>&1
            fi
            sleep 5
            continue
        fi
        if [ "$(wpa_state_now)" = "COMPLETED" ]; then
            _ssid=$(wpa_ssid_now)
            if [ -n "$_ssid" ] && [ "$_ssid" != "$_saved" ]; then
                wpa_cli -p "$WPA_CTRL" -i "$WPA_IF" save_config >/dev/null 2>&1
                # save_config 在"没有 -c"的实例上会返回 OK 但什么都不写，
                # 所以要回读确认真的落盘了。
                if [ "$(wpa_conf_field ssid "$_ssid")" = "$_ssid" ]; then
                    _saved="$_ssid"
                    _fail=0
                    echo "[init] WiFi 已保存: $_ssid"
                    sync_aka_wifi_json "$_ssid"
                else
                    _fail=$((_fail + 1))
                    echo "[init] WiFi 保存未生效 ($_fail): $_ssid"
                    [ "$_fail" -ge 3 ] && _saved="$_ssid"   # 放弃重试，避免刷屏
                fi
            fi
        elif [ "$_gave_up" = 0 ] && [ "$(date +%s)" -ge "$_deadline" ]; then
            _gave_up=1
            echo "[init] ${STA_CONNECT_TIMEOUT}s 内未连上已保存的 WiFi → 关闭自动连接（WiFi 页面仍可手动连接）"
            wpa_cli -p "$WPA_CTRL" -i "$WPA_IF" terminate >/dev/null 2>&1 || true
            # busybox 的 `ps` 不加 w 会截断命令行，匹配不到接口名 —— 用 `ps w`
            for _p in $(ps w 2>/dev/null | grep '[w]pa_supplicant' | grep "$WPA_IF" | awk '{print $1}'); do
                kill "$_p" 2>/dev/null || true
            done
            ip link set "$WPA_IF" down 2>/dev/null || true
        fi
        sleep 2
    done
) &

# 后台守护 2：eth0 死默认路由 + 开机对时
#
# /etc/network/interfaces 里 eth0 是 static + `gateway 192.168.1.1`，开机 ifup
# 会装一条 metric 0 的默认路由（即使网线没插、NO-CARRIER），它压过 wlan1 的
# metric 3004 默认路由 —— 于是 DNS/HTTPS 全走一条不通的有线网关，OTA、"检查更新"
# 都会失败。原来这里只在启动瞬间查一次，而 wlan1 要等 capp 关联后才拿到 IP，
# 条件永远不成立。现在改成常驻检查。
#
# 只删 eth0 的 **default** 路由，不碰 192.168.1.0/24 直连路由（有线调试还要用）。
# （注意：busybox 的 `ip route show default dev eth0` 会忽略 default 过滤，
#  连直连路由一起返回，所以这里自己按行过滤。）
(
    _tick=0
    while true; do
        _tick=$((_tick + 1))
        if ip -4 -o addr show "$WPA_IF" 2>/dev/null | grep -q 'inet '; then
            ip route show 2>/dev/null | while read -r _line; do
                case "$_line" in
                    default\ *dev\ eth0*)
                        ip route del $_line 2>/dev/null &&
                            echo "[init] removed eth0 default route: $_line"
                        ;;
                esac
            done
            # 时钟还停在 1970 = ntpd 还没对上时。ntpd 对不可达服务器会退避到
            # 1024s，网络刚恢复时等它自己来太慢（TLS 校验证书有效期会直接失败），
            # 每 60s 重启一次 daemon，让它立刻重新发起对时。
            if [ "$(date +%Y)" -lt 2024 ] && [ $((_tick % 20)) -eq 0 ]; then
                killall ntpd 2>/dev/null
                sleep 1
                if ntpd -g -p /var/run/ntpd.pid >/dev/null 2>&1; then
                    echo "[init] 网络就绪，重启 ntpd 立即对时"
                fi
            fi
        fi
        sleep 3
    done
) &

echo "[init] AKA-00 capp starting (AKA_HOME=$AKA_HOME)"
while true; do
    while [ -f "$LOCK_FILE" ]; do
        sleep 0.5
    done
    # PATH 前缀带上 $AKA_HOME/bin：那里是本仓自带的 wpa_cli 包装，把 wpa_cli
    # 输出里的 \xHH（非 ASCII SSID）还原成 UTF-8 —— 补上老 aka-capp 二进制缺的
    # decode_ssid。只影响 capp 及其 popen 子进程；本脚本自己的 wpa_cli 调用
    # 在下面其它位置，不受影响。
    PATH="$AKA_HOME/bin:$PATH" "$BIN" &
    _pid=$!
    echo "$_pid" > "$PID_FILE"
    echo "[init] capp pid $_pid (waiting for exit...)"
    wait "$_pid"
    echo "[init] capp exited, restarting in 2s..."
    sleep 2
done
