#!/bin/bash
# Build release packages for RK3576 / Debian 12 deployment.
#
# Usage:
#   ./build_release.sh              # both packages
#   ./build_release.sh --rebuild    # rebuild frontend then package
#
# Output:
#   dist/aka-00-server    — self-extracting for first-time deploy
#   dist/aka-ota.tar.gz   — OTA package for /api/ota/upgrade

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTPUT="$SCRIPT_DIR/dist/aka-00-server"
mkdir -p "$SCRIPT_DIR/dist"

# Build frontend if needed
if [ "$1" = "--rebuild" ] || [ ! -d "$SCRIPT_DIR/static" ] || [ -z "$(ls -A "$SCRIPT_DIR/static" 2>/dev/null)" ]; then
    echo "Building frontend..."
    cd "$SCRIPT_DIR/frontend" && npm run build
    cd "$SCRIPT_DIR"
    if [ -d "$SCRIPT_DIR/frontend/dist" ]; then
        rm -rf "$SCRIPT_DIR/static"
        cp -r "$SCRIPT_DIR/frontend/dist" "$SCRIPT_DIR/static"
        echo "Synced frontend/dist -> static/"
    fi
fi

# Create the self-extracting executable.
# Supports two modes:
#   aka-00-server          — normal run (extract if first time, then start server)
#   aka-00-server --init   — first-time setup (extract + AP hotspot + systemd)
#   aka-00-server --update — force extract + restart (for OTA)
cat > "$OUTPUT" <<'HEADER'
#!/bin/sh
# AKA-00 Server (RK3576)
set -e

APP_DIR="${AKA_HOME:-/home/cat/aka00}"

extract_payload() {
    echo "Installing AKA-00 to $APP_DIR ..."
    mkdir -p "$APP_DIR"
    sed -n '/^#__PAYLOAD_BELOW__$/,$p' "$0" | tail -n +2 | python3 -c "
import base64, sys, tarfile, tempfile, os
tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.tar.gz')
try:
    tmp.write(base64.b64decode(sys.stdin.buffer.read()))
    tmp.close()
    tarfile.open(tmp.name, mode='r:gz').extractall(path='$APP_DIR')
finally:
    os.unlink(tmp.name)
"
    chmod +x "$APP_DIR"/*.sh 2>/dev/null || true
    echo "Install done."
}

# ── --init: first-time setup ──────────────────────────────────────────
if [ "$1" = "--init" ]; then
    echo "=== AKA-00 First-Time Setup (RK3576) ==="
    extract_payload

    # 1. 安装 systemd 服务
    echo ""
    echo "Installing systemd service..."
    cp "$APP_DIR/services/aka-00.service" /etc/systemd/system/aka-00.service
    chmod 644 /etc/systemd/system/aka-00.service
    systemctl daemon-reload
    systemctl enable aka-00.service
    echo "✅ systemd service installed"

    # 2. 首次启动（init_ap_web.sh 会创建 AP 热点 + 启动 Python 服务）
    echo ""
    echo "Starting AP hotspot + web service..."
    systemctl start aka-00.service
    sleep 3

    # 3. 检查状态
    echo ""
    if systemctl is-active --quiet aka-00.service; then
        echo "============================================"
        echo "  ✅ 部署完成!"
        echo "  SSID: chenlong-robot-<id>"
        echo "  访问: http://192.168.4.1"
        echo "  管理: systemctl status aka-00"
        echo "============================================"
    else
        echo "⚠️  服务启动失败"
        journalctl -u aka-00.service --no-pager -n 15
    fi
    exit 0
fi

# ── --update: OTA upgrade ─────────────────────────────────────────────
if [ "$1" = "--update" ]; then
    echo "=== AKA-00 OTA Update ==="
    extract_payload
    # Kill waiting init.sh (from inittab), clean up, start fresh
    if [ -f /var/run/aka-init.pid ]; then
        kill $(cat /var/run/aka-init.pid) 2>/dev/null || true
        sleep 0.5
    fi
    rm -f /tmp/aka-ota-lock /tmp/aka-ota-install.sh /tmp/aka-ota-update
    cd "$APP_DIR"
    exec ./init.sh
fi

# ── Normal run ─────────────────────────────────────────────────────────
if [ ! -f "$APP_DIR/run.py" ]; then
    extract_payload
fi
cd "$APP_DIR"
exec ./init.sh
#__PAYLOAD_BELOW__
HEADER

# Generate VERSION — HEAD 有 tag 就用 tag，否则用 commit 数量
if git -C "$SCRIPT_DIR" rev-parse --git-dir >/dev/null 2>&1; then
    if tag=$(git -C "$SCRIPT_DIR" describe --tags --exact-match 2>/dev/null) && [ -n "$tag" ]; then
        ver="$tag"
    else
        count=$(git -C "$SCRIPT_DIR" rev-list --count HEAD 2>/dev/null || echo 0)
        ver="0.0.${count}"
        if ! git -C "$SCRIPT_DIR" diff-index --quiet HEAD -- 2>/dev/null; then
            ver="${ver}-dirty"
        fi
    fi
elif [ -f "$SCRIPT_DIR/VERSION" ]; then
    ver=$(head -1 "$SCRIPT_DIR/VERSION" | cut -d@ -f1)
else
    ver="0.0.0"
fi
echo "${ver}@$(date +%s)" > "$SCRIPT_DIR/VERSION"
echo "Version: ${ver}"

# Append payload (base64-encoded tar.gz)
echo "Packaging project..."
cd "$SCRIPT_DIR"
COPYFILE_DISABLE=1 tar cz \
    --exclude='dora' \
    --exclude='frontend' \
    --exclude='node_modules' \
    --exclude='dist' \
    --exclude='.git' \
    --exclude='__pycache__' \
    --exclude='*.pyc' \
    --exclude='images' \
    --exclude='docs' \
    --exclude='tests' \
    --exclude='output' \
    --exclude='checkpoints' \
    --exclude='*.zip' \
    --exclude='*.pdf' \
    --exclude='.claude' \
    --exclude='.vscode' \
    --exclude='.idea' \
    --exclude='build_release.sh' \
    --exclude='deploy_openrc_services.sh' \
    --exclude='hardware' \
    . | base64 >> "$OUTPUT"

chmod +x "$OUTPUT"
echo "Done: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"

# ---- OTA package (tar.gz) ----
if [ "$1" = "--ota" ] || [ "$2" = "--ota" ]; then
    OTA_PKG="$SCRIPT_DIR/dist/aka-ota.tar.gz"

    git -C "$SCRIPT_DIR" describe --tags --always --dirty 2>/dev/null > "$SCRIPT_DIR/VERSION" || \
    if [ -f "$SCRIPT_DIR/VERSION" ]; then
        ver=$(head -1 "$SCRIPT_DIR/VERSION" | cut -d@ -f1)
        echo "${ver}@$(date +%s)" > "$SCRIPT_DIR/VERSION"
    else
        echo "v0.1.0@$(date +%s)" > "$SCRIPT_DIR/VERSION"
    fi

    echo "Building OTA package..."
    COPYFILE_DISABLE=1 tar czf "$OTA_PKG" \
        --exclude='dora' \
        --exclude='frontend' \
        --exclude='node_modules' \
        --exclude='dist' \
        --exclude='.git' \
        --exclude='__pycache__' \
        --exclude='*.pyc' \
        --exclude='images' \
        --exclude='docs' \
        --exclude='tests' \
        --exclude='output' \
        --exclude='checkpoints' \
        --exclude='*.zip' \
        --exclude='*.pdf' \
        --exclude='.claude' \
        --exclude='.vscode' \
        --exclude='.idea' \
        --exclude='build_release.sh' \
        --exclude='deploy_openrc_services.sh' \
        --exclude='hardware' \
        .

    echo "Done: $OTA_PKG ($(du -h "$OTA_PKG" | cut -f1))"
    echo ""
    echo "Upload to GitHub Releases:"
    echo "  gh release create vX.Y.Z $OTA_PKG --title 'vX.Y.Z'"
    echo ""
    echo "Or upload manually at:"
    echo "  https://github.com/chenlongos/AKA-00/releases/new"
fi

echo ""
echo "Deploy:"
echo "  scp $OUTPUT root@<robot>:/tmp/"
echo "  ssh root@<robot> '/tmp/aka-00-server'"
echo ""
echo "OTA (self-extracting):"
echo "  上传 aka-00-server 到更新服务器，设备网页点击\"检查更新\"→\"立即升级\""
echo ""
echo "Update (push):"
echo "  scp $OUTPUT root@<robot>:/usr/local/bin/"
echo "  ssh cat@<robot> 'rm -rf /home/cat/aka00 && aka-00-server'"
echo ""
echo "Update (OTA pull):"
echo "  连接 AP 热点后访问 http://192.168.4.1/ota → 在线更新"
