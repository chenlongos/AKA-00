#!/bin/bash
# Build a standalone executable for SG2002 deployment.
# Output: dist/aka-server — copy to board, chmod +x, run directly.
#
# Usage:
#   ./build_release.sh              # build with existing static/
#   ./build_release.sh --rebuild    # rebuild frontend then package

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTPUT="$SCRIPT_DIR/dist/aka-server"
mkdir -p "$SCRIPT_DIR/dist"

# Build frontend if needed
if [ "$1" = "--rebuild" ] || [ ! -d "$SCRIPT_DIR/static" ] || [ -z "$(ls -A "$SCRIPT_DIR/static" 2>/dev/null)" ]; then
    echo "Building frontend..."
    cd "$SCRIPT_DIR/frontend" && npm run build
    cd "$SCRIPT_DIR"
fi

# Create the self-extracting executable.
# Supports two modes:
#   aka-server          — normal run (extract if first time, then start server)
#   aka-server --init   — first-time setup (extract + AP hotspot + DHCP + auto-start)
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

# Append payload (base64-encoded tar.gz)
echo "Packaging project..."
cd "$SCRIPT_DIR"
COPYFILE_DISABLE=1 tar cz \
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
    . | base64 >> "$OUTPUT"

chmod +x "$OUTPUT"
echo "Done: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
echo ""
echo "Deploy:"
echo "  scp $OUTPUT root@<robot>:/usr/local/bin/"
echo "  ssh root@<robot> 'aka-server --init'   # first time only"
echo "  ssh root@<robot> 'aka-server'          # normal run"
echo ""
echo "Update:"
echo "  ssh root@<robot> 'rm -rf /root/AKA-00'  # then copy and run again"
