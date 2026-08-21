#!/bin/bash
# AKA-00 startup (RK3576)
# 由 systemd aka-00.service 调用，或直接执行
# UART 初始化已在 RK3576 内核/设备树中完成，无需额外操作

APP_DIR="${AKA_HOME:-/home/cat/aka00}"
LOCK_FILE="/tmp/aka-ota-lock"
cd "$APP_DIR"

# 激活 venv 环境
VENV_DIR="/home/cat/venv_rk3576"
if [ -f "$VENV_DIR/bin/activate" ]; then
    source "$VENV_DIR/bin/activate"
fi

# run.py 内部自动生成 SSL 证书、启动 HTTP/HTTPS 服务
# OTA 更新进行中时等待其完成，避免与更新流程冲突
while [ -f "$LOCK_FILE" ]; do
    sleep 0.5
done
exec python "$APP_DIR/run.py"
