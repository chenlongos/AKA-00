#!/bin/sh
# dora debug demo — 启动完整调试数据流（摄像头 + 网页 + 电机控制）
#
# 使用方式：
#   cd dora/demo-debug && ./init.sh
#   浏览器打开 http://localhost:8080
#   停止：./stop.sh
#
# 对应 demo/tennis/init.sh 的模式

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
DORA_DIR="$(cd "$DIR/.." && pwd)"

echo "╔══════════════════════════════════╗"
echo "║   dora debug demo               ║"
echo "╚══════════════════════════════════╝"

# ── 1. 编译 ──
echo ""
echo "🔨 Building nodes..."
cd "$DORA_DIR"

cargo build -p web-server --release 2>&1 | grep -E "Compiling|Finished|error" || true
cargo build -p motor-bridge --release 2>&1 | grep -E "Compiling|Finished|error" || true
make -C camera-node 2>&1 | grep -E "error|warning" | head -3 || true

echo "   ✅ build done"

# ── 2. 启动 dora ──
echo ""
echo "🚀 Starting dora runtime..."
dora up 2>/dev/null

# ── 3. 运行数据流 ──
echo ""
echo "📡 Launching dataflow..."
echo ""
dora run "$DORA_DIR/dataflow.yml" &
DORA_PID=$!

# ── 4. 等待就绪 ──
echo "⏳ Waiting for web-server..."
for i in $(seq 1 15); do
    sleep 0.5
    if curl -s http://localhost:8080/api/camera/status > /dev/null 2>&1; then
        echo ""
        echo "╔══════════════════════════════════╗"
        echo "║  🟢 Demo ready!                 ║"
        echo "║                                ║"
        echo "║  📷 http://localhost:8080       ║"
        echo "║  📊 http://localhost:8080/api/camera/status"
        echo "║  🔧 http://localhost:8080/api/motor/status"
        echo "║                                ║"
        echo "║  Stop: ./stop.sh               ║"
        echo "╚══════════════════════════════════╝"
        exit 0
    fi
    printf "."
done

echo ""
echo "⚠️  web-server not responding. Check: dora logs"
exit 1
