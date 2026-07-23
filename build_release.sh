#!/bin/bash
# Build release packages for SG2002 deployment.
#
# Usage:
#   ./build_release.sh              # aka-00-server (self-extracting, for scp deploy)
#   ./build_release.sh --rebuild    # rebuild frontend then package
#   ./build_release.sh --ota        # also generate aka-ota.tar.gz (for OTA pull)
#
# Output:
#   dist/aka-00-server     — self-extracting executable for first-time deploy
#   dist/aka-ota.tar.gz — OTA package for /api/ota/upgrade (upload to GitHub Releases)

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
#   aka-00-server          — normal run (extract if first time, then start server)
#   aka-00-server --init   — first-time setup (extract + AP hotspot + DHCP + auto-start)
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

# ── Normal run ─────────────────────────────────────────────────────────
if [ ! -f "$APP_DIR/run.py" ]; then
    extract_payload
fi
cd "$APP_DIR"
exec ./init.sh
#__PAYLOAD_BELOW__
HEADER

# Generate VERSION (for both build modes)
echo "$(date +%s)" > "$SCRIPT_DIR/VERSION"

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

# Clean up temp VERSION
rm -f "$SCRIPT_DIR/VERSION"

# ---- OTA package (tar.gz) ----
if [ "$1" = "--ota" ] || [ "$2" = "--ota" ]; then
    OTA_PKG="$SCRIPT_DIR/dist/aka-ota.tar.gz"

    # Generate VERSION file from git tag
    git -C "$SCRIPT_DIR" describe --tags --always --dirty 2>/dev/null > "$SCRIPT_DIR/VERSION" || \
        echo "dev-$(date +%Y%m%d%H%M)" > "$SCRIPT_DIR/VERSION"

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
        --exclude='hardware' \
        .

    # Clean up temp VERSION file (it's baked into the tar.gz)
    rm -f "$SCRIPT_DIR/VERSION"

    echo "Done: $OTA_PKG ($(du -h "$OTA_PKG" | cut -f1))"
    echo ""
    echo "Upload to GitHub Releases:"
    echo "  gh release create vX.Y.Z $OTA_PKG --title 'vX.Y.Z'"
    echo ""
    echo "Or upload manually at:"
    echo "  https://github.com/chenlongos/AKA-00/releases/new"
fi

echo ""
echo "Deploy (first time):"
echo "  scp $OUTPUT root@<robot>:/usr/local/bin/"
echo "  ssh root@<robot> 'aka-00-server --init'   # first time only"
echo ""
echo "Update (push):"
echo "  scp $OUTPUT root@<robot>:/usr/local/bin/"
echo "  ssh root@<robot> 'rm -rf /root/AKA-00 && aka-00-server'"
echo ""
echo "Update (OTA pull):"
echo "  机器人连接 http://192.168.4.1/ota → 在线更新"
