#!/bin/sh
# =============================================================================
# dora 本地开发模式 — 完整 dataflow，全部启动
#
# 与 dora/init.sh 区别：
#   - 从 dora/config.toml 读全部配置（motor / arm backend、web port、log level）
#   - 自动设 DEMO_BASE_DIR（脚本相对 ../demo）和 ARM_ANGLES（同上 arm_angles.json）
#   - 跳过 uart_init.sh（板子专属，本机没 /dev/ttyS*）
#   - 不做 RISC-V 交叉编译（build_release.sh 干那个）
#   - cargo build 用 host target
#
# 用法:
#   cd dora && ./dev.sh
#
# 可覆盖的环境变量（脚本值作 fallback）:
#   DEMO_BASE_DIR    demo 子目录绝对路径
#   ARM_ANGLES       机械臂角度配置文件绝对路径
#   RUST_LOG         日志级别（默认读 config.toml [logging] level）
#
# 停止:
#   ./stop.sh  (或 Ctrl-C 一次再清残留)
# =============================================================================

set -e
# pipefail 没开的话，"cargo build 2>&1 | grep ..." 这种管线会只看 grep 的退出码，
# cargo 自身的失败会被吞掉，dev.sh 假装"build done"其实编失败了。一行的修复：
# 一旦 cargo 退出非 0，整个管线退出非 0，set -e 会触发中断。
set -o pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

# ── 帮助文本 ──
usage() {
    cat <<'USAGE'
dora 本地开发模式

用法:
    ./dev.sh                       # 默认按 config.toml 启
    DEMO_BASE_DIR=... ./dev.sh     # 临时改 demo 路径
    RUST_LOG=debug ./dev.sh        # 临时开 debug 日志

USAGE
    exit 0
}
case "${1:-}" in -h|--help|help) usage ;; esac

echo "╔══════════════════════════════════╗"
echo "║   dora robot system (LOCAL)     ║"
echo "╚══════════════════════════════════╝"

# ── 1. 从 config.toml 读配置 ──
# 简单 grep + sed 解析（config.toml 单 key 单 value，没引用、没嵌套）。
# 复杂解析交给 toml crate 反而重；这里够用。
CONFIG="config.toml"

read_toml() {
    # read_toml <section> <key> [<default>]
    section="$1"; key="$2"; default="${3:-}"
    val=$(awk -v sec="[$section]" -v k="$key" '
        $0 == sec { in_sec=1; next }
        in_sec && /^\[/ { in_sec=0 }
        in_sec && $0 ~ "^"k"[[:space:]]*=" {
            sub("^"k"[[:space:]]*=[[:space:]]*", "")
            sub("[[:space:]]*$", "")
            gsub(/^"|"$/, "")
            print; exit
        }
    ' "$CONFIG" 2>/dev/null)
    [ -n "$val" ] && echo "$val" || echo "$default"
}

MOTOR_BACKEND=$(read_toml motor backend dev)
ARM_BACKEND=$(read_toml arm backend dev)
WEB_PORT=$(read_toml web port 80)
LOG_LEVEL=$(read_toml logging level info)
CAM_WIDTH=$(read_toml camera width 640)
CAM_HEIGHT=$(read_toml camera height 480)

# demo + arm_angles 默认值: 脚本所在目录的父级 (workspace 根) 的 demo / arm_angles.json。
# 这是 monorepo 布局：dora/ 与 demo/ arm_angles.json 是 workspace 根的同级。
DEMO_DEFAULT="$DIR/../demo"
ARM_DEFAULT="$DIR/arm_angles.json"

# 环境变量已设则沿用，否则用绝对化的默认，避免 web-server/dora-node 的 cwd 不对导致路径错
export DEMO_BASE_DIR="${DEMO_BASE_DIR:-$(cd "$DEMO_DEFAULT" 2>/dev/null && pwd || echo "$DEMO_DEFAULT")}"
# arm_angles.json 现在跟 dev.sh 同目录（dora/arm_angles.json），是 web-server 的真源。
# 注意环境变量名是 ARM_ANGLES_PATH（不是 ARM_ANGLES——后者曾被静默忽略，是 bug）。
export ARM_ANGLES_PATH="${ARM_ANGLES_PATH:-$(cd "$(dirname "$ARM_DEFAULT")" 2>/dev/null && pwd || echo "$ARM_DEFAULT")}/$(basename "$ARM_DEFAULT")"
if [ -f "$ARM_ANGLES_PATH" ]; then
    export ARM_ANGLES_DIR="$(dirname "$ARM_ANGLES_PATH")"
fi

# 下发到 web-server / camera-node / demo-node
export RUST_LOG="${RUST_LOG:-$LOG_LEVEL}"
export CAMERA_LOG_LEVEL="${CAMERA_LOG_LEVEL:-$LOG_LEVEL}"
export CAMERA_WIDTH="${CAMERA_WIDTH:-$CAM_WIDTH}"
export CAMERA_HEIGHT="${CAMERA_HEIGHT:-$CAM_HEIGHT}"

# dora 路径 — host 装的 dora CLI 在 PATH
DORA_BIN="$(command -v dora 2>/dev/null || true)"
if [ -z "$DORA_BIN" ]; then
    echo ""
    echo "❌ 'dora' CLI 不在 PATH。装一下："
    echo "    curl -sSf https://dora-rs.ai/install.sh | sh"
    echo "  然后确保 \$HOME/.cargo/bin 或 \$DORA_HOME/bin 在 PATH。"
    exit 1
fi

echo ""
echo "   motor backend:   $MOTOR_BACKEND"
echo "   arm backend:     $ARM_BACKEND"
echo "   web port:        $WEB_PORT"
echo "   camera:          ${CAMERA_WIDTH}x${CAMERA_HEIGHT}"
echo "   log level:       $RUST_LOG"
echo "   demo base dir:   $DEMO_BASE_DIR"
echo "   arm angles:      $ARM_ANGLES_PATH"
echo "   dora CLI:        $DORA_BIN"

if [ ! -d "$DEMO_BASE_DIR" ]; then
    warn() { echo "   ⚠  $1"; }
    warn "DEMO_BASE_DIR 不存在 ($DEMO_BASE_DIR) — demo 列表会空"
fi

# ── 2. 编译 ──
echo ""
echo "🔨 Building (host target)..."
# 让 dora run 自己触发 build 也不是不行，但手动 build 一下能"快速失败"——
# 编译错误不会卡在奇怪的 dora 错误上。
build_crate() {
    cargo build -p "$1" --release 2>&1 \
        | grep -E "Compiling|Finished|error" \
        | sed 's/^/    /' \
        || true
}
build_crate dora-c-ffi        # camera-node 的 Makefile 链 libdora_c_ffi.a，必须先出 staticlib
build_crate web-server
build_crate motor-bridge
build_crate arm-bridge
build_crate demo-node
build_crate state-node

# camera-node 是 C++，走 make
if [ -f camera-node/Makefile ] && [ -f camera-node/main.cc ]; then
    echo "   building camera-node (make)..."
    if ! make -C camera-node 2>&1 | grep -E "error" | sed 's/^/    /'; then
        echo "   ✅ camera-node built"
    else
        echo "   ⚠ camera-node build failed — 摄像头节点将启动失败"
    fi
elif [ -d camera-node ]; then
    echo "   ⚠ camera-node 没有 Makefile — 跳过（可能是 src 路径变化）"
fi

# 探测 demo 二进制能不能找到（demo 子目录里通常有 ./tennis 之类）
if [ -d "$DEMO_BASE_DIR" ]; then
    demo_count=$(find "$DEMO_BASE_DIR" -maxdepth 2 -name "init.sh" 2>/dev/null | wc -l | tr -d ' ')
    echo "   demos found: $demo_count 个 (有 init.sh 的子目录)"
fi

echo "   ✅ build phase done"

# ── 3. dora 守护进程 ──
echo ""
echo "🚀 Starting dora daemon..."
"$DORA_BIN" up 2>/dev/null || {
    rc=$?
    if [ "$rc" != "0" ]; then
        echo "   ⚠ 'dora up' 返回 $rc — 继续尝试，已有 daemon 可能够用"
    fi
}

# ── 4. dataflow ──
echo ""
echo "📡 Launching dataflow..."
# 用普通 `&`：dora run 继承 dev.sh 的 PGID，trap 里 `kill -INT -$$`
# （负号 = 杀整个进程组）就能一次清掉 shell + dora run + 所有 dora 子节点。
# 不用 setsid（macOS 没这个命令；Linux 上是个可移植优化，但 std `&` 也够用）。
"$DORA_BIN" run dataflow.yml >"$DIR/.dora.log" 2>&1 &
DORA_PID=$!

# SIGINT 透传给整个 dev.sh 进程组（含 dora run + coordinator + 各 dataflow node），
# Ctrl-C 时把所有 dora 相关进程一起收掉，避免孤儿占端口。
trap 'echo ""; echo "🛑 Caught signal, stopping..."; kill -INT "-$$" 2>/dev/null || true; exit 130' INT TERM

# ── 5. 等待 web-server 就绪 ──
echo ""
echo "⏳ Waiting for web-server on :$WEB_PORT ..."
ready=0
for i in $(seq 1 30); do
    sleep 1
    if curl -fs "http://localhost:${WEB_PORT}/api/camera/status" > /dev/null 2>&1; then
        ready=1
        break
    fi
    printf "."
done
if [ "$ready" != "1" ]; then
    echo ""
    echo "❌ web-server 没起来。最后 30 行日志："
    tail -30 "$DIR/.dora.log" 2>/dev/null || true
    echo ""
    echo "手动排查："
    echo "    tail -f $DIR/.dora.log"
    echo "    dora logs"
    exit 1
fi

echo ""
echo "╔════════════════════════════════════════════════╗"
echo "║  🟢 Ready                                      ║"
echo "║                                                ║"
echo "║  📷 UI   http://localhost:${WEB_PORT}               ║"
echo "║  📋 log  tail -f $DIR/.dora.log                    ║"
echo "║  🛑 stop  ./stop.sh  或 Ctrl-C                  ║"
echo "╚════════════════════════════════════════════════╝"

# 前台等 dora run，Ctrl-C 进 trap
wait "$DORA_PID"
