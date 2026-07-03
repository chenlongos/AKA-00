#!/bin/sh
set -e
echo "🛑 Stopping dora..."
dora stop 2>/dev/null || true
dora destroy 2>/dev/null || true
for name in camera-node web-server motor-bridge state-node dora-daemon dora-coordinator; do
    pgrep -f "$name" 2>/dev/null | while read pid; do [ -n "$pid" ] && kill "$pid" 2>/dev/null; done
done
sleep 0.3
for name in camera-node web-server motor-bridge state-node dora-daemon dora-coordinator; do
    pgrep -f "$name" 2>/dev/null | while read pid; do [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null; done
done
lsof -ti :80 2>/dev/null | while read pid; do [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null; done

# 清理 dora 共享内存残留（防止下次启动变慢）
rm -f /dev/shm/dora-* 2>/dev/null || true
rm -f /dev/shm/zenoh-* 2>/dev/null || true

echo "✅ Demo stopped"
