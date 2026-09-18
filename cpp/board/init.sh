#!/bin/sh
# =============================================================================
# AKA-00 capp 启动脚本（SG2002 板子）
#
# 自愈循环：capp 崩溃自动重启。OTA 进行中（/tmp/aka-ota-lock 存在）时等待。
#
# 用法:
#   AKA_HOME=$HOME/AKA-00 ./init.sh
# =============================================================================

AKA_HOME="${AKA_HOME:-$HOME/AKA-00}"
BIN="$AKA_HOME/aka-capp"
LOCK_FILE="/tmp/aka-ota-lock"
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

# WiFi 已连接时兜底删除 eth0 默认路由：
# 有线 static 网关（如 /etc/network/interfaces 的 gateway 192.168.1.1）会让默认
# 路由走 eth0，WiFi 客户端访问板子的回包走有线网关 → 不对称路由 → 连不上。
# 正解是注释 /etc/network/interfaces 的 auto eth0（开机不再配）；这里是双保险，
# 每次启动 capp 前检查并清掉。
if ip -4 -o addr show wlan1 2>/dev/null | grep -q 'inet '; then
    ip route show default dev eth0 2>/dev/null | while read -r line; do
        ip route del $line 2>/dev/null && echo "[init] removed eth0 default route: $line"
    done
fi

echo "[init] AKA-00 capp starting (AKA_HOME=$AKA_HOME)"
while true; do
    while [ -f "$LOCK_FILE" ]; do
        sleep 0.5
    done
    "$BIN" &
    _pid=$!
    echo "$_pid" > "$PID_FILE"
    echo "[init] capp pid $_pid (waiting for exit...)"
    wait "$_pid"
    echo "[init] capp exited, restarting in 2s..."
    sleep 2
done
