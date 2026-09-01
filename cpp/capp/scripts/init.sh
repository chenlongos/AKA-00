#!/bin/sh
# =============================================================================
# AKA-00 capp 启动脚本（SG2002 板子）
#
# 自愈循环：capp 崩溃自动重启。OTA 进行中（/tmp/aka-ota-lock 存在）时等待。
#
# 用法:
#   AKA_HOME=/root/AKA-00 ./init.sh
# =============================================================================

APP_DIR="${AKA_HOME:-/root/AKA-00}"
BIN="$APP_DIR/aka-capp"
LOCK_FILE="/tmp/aka-ota-lock"
PID_FILE="/var/run/aka-capp.pid"

# 防止重复实例（init.sh + 手动启动）
if [ -f "$PID_FILE" ]; then
    _old=$(cat "$PID_FILE")
    if kill -0 "$_old" 2>/dev/null; then
        echo "[init] already running (pid $_old), exiting"
        exit 0
    fi
fi

if [ ! -x "$BIN" ]; then
    # 兜底：scp/tar/zip 传输可能丢可执行位
    chmod +x "$BIN" 2>/dev/null || true
fi
if [ ! -x "$BIN" ]; then
    echo "[init] $BIN not found or not executable"
    exit 1
fi

# 摄像头分辨率环境（可选，config.toml 也控制）
CAM_WIDTH=$(grep -E '^\s*width\s*=' "$APP_DIR/config.toml" 2>/dev/null | grep -oE '[0-9]+' | head -1)
CAM_HEIGHT=$(grep -E '^\s*height\s*=' "$APP_DIR/config.toml" 2>/dev/null | grep -oE '[0-9]+' | head -1)
[ -n "$CAM_WIDTH" ] && export CAMERA_WIDTH="$CAM_WIDTH"
[ -n "$CAM_HEIGHT" ] && export CAMERA_HEIGHT="$CAM_HEIGHT"

# 显示引擎（可选，有屏才需要）
if [ -w /sys/class/graphics/fb0/state ]; then
    echo 1 > /sys/class/graphics/fb0/state 2>/dev/null || true
    echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null || true
fi

# 关键：export AKA_HOME，否则 capp 的 static/config 路径退化为 cwd 相对路径，
# 从别的目录启动就找不到 static/ 和 config.toml。
export AKA_HOME="$APP_DIR"

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

echo "[init] AKA-00 capp starting (AKA_HOME=$APP_DIR)"
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
