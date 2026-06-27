#!/bin/bash
set -e

echo "🧹 Stopping all dora services..."

# 1. 优雅停止 dora 数据流和守护进程
dora stop   2>/dev/null || true
dora destroy 2>/dev/null || true

# 2. 杀掉残留的 dora 节点进程
NODES=(
    "camera-node"
    "display-node"
    "web-server"
    "dora-daemon"
    "dora-coordinator"
    "dora-runtime"
)

for name in "${NODES[@]}"; do
    killed=0
    while read pid; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null && killed=$((killed + 1))
        fi
    done < <(pgrep -f "$name" 2>/dev/null || true)
    [ "$killed" -gt 0 ] && echo "  └ killed $killed × $name"
done

# 3. 强制清理残留
sleep 0.3
for name in "${NODES[@]}"; do
    while read pid; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null || true
    done < <(pgrep -f "$name" 2>/dev/null || true)
done

# 4. 释放端口 8080
PORT=8080
while read pid; do
    if [ -n "$pid" ]; then
        kill -9 "$pid" 2>/dev/null && echo "  └ freed port $PORT (pid $pid)"
    fi
done < <(lsof -ti :$PORT 2>/dev/null || true)

echo "✅ Done"
