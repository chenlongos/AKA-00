#!/bin/bash
# Build release packages for SG2002 deployment.
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
# Frontend 构建输出在 dora/web-server/static（供 dora dev 模式使用），
# 同时同步到根 static/ 供 Python Flask 生产部署使用。
if [ "$1" = "--rebuild" ] || [ ! -d "$SCRIPT_DIR/static" ] || [ -z "$(ls -A "$SCRIPT_DIR/static" 2>/dev/null)" ]; then
    echo "Building frontend..."
    cd "$SCRIPT_DIR/frontend" && npm run build
    cd "$SCRIPT_DIR"
    # 同步到根 static/ 目录（生产部署需要）
    if [ -d "$SCRIPT_DIR/dora/web-server/static" ]; then
        rm -rf "$SCRIPT_DIR/static"
        cp -r "$SCRIPT_DIR/dora/web-server/static" "$SCRIPT_DIR/static"
        echo "Synced dora/web-server/static -> static/"
    fi
fi

# Create the self-extracting executable.
# Supports two modes:
#   aka-00-server              — normal run (extract if first time, then start)
#   aka-00-server --init       — first-time setup (extract + AP hotspot + DHCP)
#   aka-00-server --update     — force extract + restart (for OTA)
cat > "$OUTPUT" <<'HEADER'
#!/bin/sh
# AKA-00 Server
set -e

APP_DIR="${AKA_HOME:-/root/AKA-00}"

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
    echo "=== AKA-00 First-Time Setup ==="
    extract_payload
    "$APP_DIR/init_ap_web.sh"
    echo ""
    echo "Setup complete."
    echo "  Service will auto-start on next boot."
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
    --exclude='hardware' \
    . | base64 >> "$OUTPUT"

chmod +x "$OUTPUT"
echo "Done: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"

echo ""
echo "Deploy:"
echo "  scp $OUTPUT root@<robot>:/tmp/"
echo "  ssh root@<robot> '/tmp/aka-00-server'"
echo ""
echo "OTA:"
echo "  上传 aka-00-server 到更新服务器，设备网页点击"检查更新"→"立即升级""
