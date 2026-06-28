#!/bin/sh
# dora 一键启动 — 摄像头 + 网页 + 电机控制
#
# 使用：cd dora && ./init.sh
# 停止：./stop.sh

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

echo "╔══════════════════════════════════╗"
echo "║   dora robot system             ║"
echo "╚══════════════════════════════════╝"

BACKEND=$(grep 'backend' config.toml 2>/dev/null | head -1 | cut -d'"' -f2)
echo "   motor backend: ${BACKEND:-dev}"

# ── 1. 编译 ──
echo ""
echo "🔨 Building..."
cargo build -p web-server --release 2>&1 | grep -E "Compiling|Finished|error" || true
cargo build -p motor-bridge --release 2>&1 | grep -E "Compiling|Finished|error" || true
cargo build -p state-node --release 2>&1 | grep -E "Compiling|Finished|error" || true
make -C camera-node 2>&1 | grep -E "error|warning" | head -3 || true
echo "   ✅ done"

# ── 2. 启动 ──
echo ""
echo "🚀 Starting dora..."
dora up 2>/dev/null

echo "📡 Launching dataflow..."
dora run dataflow.yml &
DORA_PID=$!

# ── 3. 等待就绪 ──
echo "⏳ Waiting for web-server..."
for i in $(seq 1 15); do
    sleep 0.5
    if curl -s http://localhost:8080/api/camera/status > /dev/null 2>&1; then
        echo ""
        echo "╔══════════════════════════════════╗"
        echo "║  🟢 Ready!                      ║"
        echo "║                                ║"
        echo "║  📷 http://localhost:8080       ║"
        echo "║  Stop: ./stop.sh               ║"
        echo "╚══════════════════════════════════╝"
        exit 0
    fi
    printf "."
done
echo ""
echo "⚠️  web-server not responding. Check: dora logs"
exit 1
