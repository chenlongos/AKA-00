#!/bin/sh
# 停止 dora debug demo
# 对应 demo/tennis/ 的停止模式

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
DORA_DIR="$(cd "$DIR/.." && pwd)"

echo "🛑 Stopping dora debug demo..."

# 1. 优雅停止
dora stop   2>/dev/null || true
dora destroy 2>/dev/null || true

# 2. 确保残留进程终止
NODES="camera-node web-server motor-bridge dora-daemon dora-coordinator"
for name in $NODES; do
    pgrep -f "$name" 2>/dev/null | while read pid; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
done

sleep 0.3
for name in $NODES; do
    pgrep -f "$name" 2>/dev/null | while read pid; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null
    done
done

# 3. 释放端口
lsof -ti :8080 2>/dev/null | while read pid; do
    [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null
done

echo "✅ Demo stopped"
