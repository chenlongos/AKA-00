#!/bin/bash
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

echo "🔨 Building..."
cargo build -p web-server --release 2>&1 | tail -1
make -C camera-node 2>&1 | tail -1

echo "🚀 Starting dora..."
dora up 2>/dev/null
dora run dataflow.yml &

# 等 web-server 就绪
echo -n "⏳ Waiting for web-server..."
for i in $(seq 1 10); do
    sleep 0.5
    if curl -s http://localhost:8080/api/status > /dev/null 2>&1; then
        echo " ready!"
        echo ""
        echo "  📷 Camera preview: http://localhost:8080"
        echo "  📊 Status:        http://localhost:8080/api/status"
        echo ""
        echo "  Stop:  ./stop.sh"
        exit 0
    fi
    echo -n "."
done

echo ""
echo "⚠️  web-server didn't respond in 5s. Check logs: dora logs"
