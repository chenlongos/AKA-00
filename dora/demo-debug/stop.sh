#!/bin/sh
set -e
echo "🛑 Stopping dora demo..."
dora stop 2>/dev/null || true
dora destroy 2>/dev/null || true
for name in camera-node web-server motor-bridge state-node dora-daemon dora-coordinator; do
    pgrep -f "$name" 2>/dev/null | while read pid; do [ -n "$pid" ] && kill "$pid" 2>/dev/null; done
done
sleep 0.3
for name in camera-node web-server motor-bridge state-node dora-daemon dora-coordinator; do
    pgrep -f "$name" 2>/dev/null | while read pid; do [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null; done
done
lsof -ti :8080 2>/dev/null | while read pid; do [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null; done
echo "✅ Demo stopped"
