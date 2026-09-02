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
export AKA_HOME
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
