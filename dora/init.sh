#!/bin/sh
# dora 一键启动 — 摄像头 + 网页 + 电机控制（本地 host 模式）
#
# 用法: cd dora && ./init.sh
# 停止: ./stop.sh
#
# 与 dev.sh 区别:
#   - init.sh 是旧的快速启（只读 motor backend，端口写死 80，15*0.5s 就绪判断）
#   - dev.sh 是新的配置驱动版（读全部 backend + port + 日志级别，30*1s，含 sigint trap）
# 如果你是第一次跑，推荐用 ./dev.sh。

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

echo "╔══════════════════════════════════╗"
echo "║   dora robot system             ║"
echo "╚══════════════════════════════════╝"

# 读 [motor] backend（保留旧 grep 风格，简单够用）
BACKEND=$(grep 'backend' config.toml 2>/dev/null | head -1 | cut -d'"' -f2)
echo "   motor backend: ${BACKEND:-dev}"

# 读 [web] port（之前写死 80，与 config.toml [web] port 冲突会让 ready 永远失败）
WEB_PORT=$(grep -A 1 '^\[web\]' config.toml 2>/dev/null | grep -oE '[0-9]+' | head -1)
WEB_PORT="${WEB_PORT:-80}"
echo "   web port:      $WEB_PORT"

# arm_angles.json 跟 init.sh 同目录（dora/arm_angles.json），是 web-server 的读写真源
export ARM_ANGLES_PATH="${ARM_ANGLES_PATH:-$DIR/arm_angles.json}"
echo "   arm angles:    $ARM_ANGLES_PATH"

# ── 1. 编译 ──
echo ""

echo "🔨 Building..."
cargo build -p web-server --release 2>&1 | grep -E "Compiling|Finished|error" || true
cargo build -p motor-bridge --release 2>&1 | grep -E "Compiling|Finished|error" || true
cargo build -p state-node --release 2>&1 | grep -E "Compiling|Finished|error" || true
make -C camera-node 2>&1 | grep -E "error|warning" | head -3 || true
echo "   ✅ done"

# ── 1.5. 日志级别 ──
CAM_LOG_LEVEL=$(grep -A 5 '^\[logging\]' config.toml 2>/dev/null \
    | grep -E '^\s*level\s*=' | head -1 | sed -E 's/.*"([^"]+)".*/\1/')
export CAMERA_LOG_LEVEL="${CAMERA_LOG_LEVEL:-info}"
echo "   camera log level: ${CAMERA_LOG_LEVEL}"

# ── 2. dora 守护进程（已有则跳过，避免 set -e 触发 abort） ──
# 用 || echo 兜底：dora up 失败（端口占用、daemon missing 等）不致命，
# dora run 可能已经能独立工作。
echo ""
echo "🚀 Starting dora..."
if ! dora up 2>/dev/null; then
    echo "   ⚠  dora up failed, assuming daemon already up — continuing"
fi

echo "📡 Launching dataflow..."
dora run dataflow.yml >"$DIR/.dora.log" 2>&1 &
DORA_PID=$!

# Ctrl-C 也走一遍 stop.sh（不然 dataflow 留着占端口）
trap 'echo ""; echo "🛑 Caught signal, stopping..."; ./stop.sh >/dev/null 2>&1 || true; exit 130' INT TERM

# ── 3. 等待就绪（30s，之前是 7.5s，首次 build 太紧） ──
echo ""
echo "⏳ Waiting for web-server on :$WEB_PORT ..."
ready=0
for i in $(seq 1 30); do
    sleep 1
    # curl -fs 让 5xx 也算失败，避免假"Ready"
    if curl -fs "http://localhost:${WEB_PORT}/api/camera/status" > /dev/null 2>&1; then
        ready=1
        break
    fi
    printf "."
done
if [ "$ready" != "1" ]; then
    echo ""
    echo "❌ web-server 没起来。最后 30 行日志："
    tail -30 "$DIR/.dora.log" 2>/dev/null || true
    exit 1
fi

echo ""
echo "╔══════════════════════════════════╗"
echo "║  🟢 Ready!                       ║"
echo "║                                  ║"
echo "║  📷 http://localhost:${WEB_PORT}  ║"
echo "║  Stop: ./stop.sh  或 Ctrl-C      ║"
echo "╚══════════════════════════════════╝"

wait "$DORA_PID"
