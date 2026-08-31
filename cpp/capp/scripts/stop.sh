#!/bin/sh
# AKA-00 capp 停止脚本：SIGTERM 优雅退出（停电机、关摄像头、落日志）

PID_FILE="/var/run/aka-capp.pid"

if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    rm -f "$PID_FILE"
    if kill -0 "$PID" 2>/dev/null; then
        echo "[stop] sending SIGTERM to $PID ..."
        kill -TERM "$PID"
        # 最多等 5 秒，未退出升级 SIGKILL
        for _ in $(seq 1 25); do
            kill -0 "$PID" 2>/dev/null || break
            sleep 0.2
        done
        if kill -0 "$PID" 2>/dev/null; then
            echo "[stop] no response, SIGKILL"
            kill -9 "$PID"
        fi
        echo "[stop] stopped"
        exit 0
    fi
fi
echo "[stop] not running"
exit 0
