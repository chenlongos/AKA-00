#!/bin/bash
# =============================================================================
# dora 交叉编译打包脚本
#
# 将 dora 数据流系统交叉编译为 riscv64 musl 静态二进制，
# 打包为 tar.gz 部署到 SG2002 (CV1812) 板子上。
#
# 用法:
#   ./build_release.sh                    # 全部编译 + 打包
#   ./build_release.sh --skip-camera      # 跳过 camera-node (无可用工具链时)
#   ./build_release.sh --deploy <host>    # 编译 + 打包 + scp 部署
#   ./build_release.sh --clean            # 清理构建产物
#
# 环境变量 (带默认值):
#   RISCV64_TOOLCHAIN    RISC-V 交叉工具链路径
#   RISCV64_OPENCV       OpenCV 安装路径 (需要静态库 .a)
#   RISCV64_SYSROOT      musl sysroot 路径
#   DORA_REF             dora-rs 源码版本 tag
#   ORBSTACK_MACHINE     orbstack 机器名 (默认空 = 默认机器)
#
# =============================================================================

set -euo pipefail

# ── 路径和常量 ────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTPUT_DIR="$SCRIPT_DIR/dist"
BUILD_DIR="$SCRIPT_DIR/build-cross"
PACKAGE_NAME="dora-riscv64"
PACKAGE_DIR="$OUTPUT_DIR/$PACKAGE_NAME"

TARGET="riscv64gc-unknown-linux-musl"
DORA_REF="${DORA_REF:-v0.5.0}"

# 工具链路径 (通过 orbstack 访问)
RISCV64_TOOLCHAIN="${RISCV64_TOOLCHAIN:-/home/junbo_dai/riscv64-linux-musl-x86_64}"
RISCV64_OPENCV="${RISCV64_OPENCV:-/home/junbo_dai/cvitek_tpu_sdk/opencv}"
RISCV64_SYSROOT="${RISCV64_SYSROOT:-${RISCV64_TOOLCHAIN}/sysroot}"

RISCV64_GCC="${RISCV64_TOOLCHAIN}/bin/riscv64-unknown-linux-musl-gcc"
RISCV64_GXX="${RISCV64_TOOLCHAIN}/bin/riscv64-unknown-linux-musl-g++"

ORB="${ORB:-orb}"
ORB_MACHINE="${ORBSTACK_MACHINE:-}"

# 控制开关
SKIP_CAMERA=false
SKIP_DORA_RUNTIME=false
DEPLOY_HOST=""
CLEAN_FIRST=false

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

# ── 参数解析 ──────────────────────────────────────────────────────────────────

for arg in "$@"; do
    case "$arg" in
        --skip-camera)    SKIP_CAMERA=true ;;
        --skip-dora)      SKIP_DORA_RUNTIME=true ;;
        --deploy)
            DEPLOY_HOST="${2:-}"
            if [ -z "$DEPLOY_HOST" ]; then
                echo "Usage: $0 --deploy <host>"
                exit 1
            fi
            shift
            ;;
        --clean)          CLEAN_FIRST=true ;;
        --help|-h)
            echo "Usage: $0 [--skip-camera] [--skip-dora] [--deploy <host>] [--clean]"
            echo ""
            echo "Environment variables:"
            echo "  RISCV64_TOOLCHAIN   default: /home/junbo_dai/riscv64-linux-musl-x86_64"
            echo "  RISCV64_OPENCV      default: /home/junbo_dai/cvitek_tpu_sdk/opencv"
            echo "  ORBSTACK_MACHINE    default: (default orbstack machine)"
            exit 0
            ;;
    esac
done

# ── 工具函数 ──────────────────────────────────────────────────────────────────

# 在 orbstack 中执行命令，如果 orb 不可用则直接执行
_orb() {
    if command -v orb &>/dev/null && orb status &>/dev/null 2>&1; then
        if [ -n "$ORB_MACHINE" ]; then
            orb -m "$ORB_MACHINE" "$@"
        else
            orb "$@"
        fi
    else
        "$@"
    fi
}

# 检查 orbstack 中路径是否存在
_orb_test() {
    if command -v orb &>/dev/null && orb status &>/dev/null 2>&1; then
        if [ -n "$ORB_MACHINE" ]; then
            orb -m "$ORB_MACHINE" test "$@"
        else
            orb test "$@"
        fi
    else
        test "$@"
    fi
}

# 打印阶段标题
phase() {
    echo ""
    echo -e "${BLUE}${BOLD}── [$1] ──────────────────────────────────────────────${NC}"
}

# 打印成功
ok() {
    echo -e "  ${GREEN}[OK]${NC} $1"
}

# 打印警告
warn() {
    echo -e "  ${YELLOW}[WARN]${NC} $1"
}

# 打印跳过
skip() {
    echo -e "  ${YELLOW}[SKIP]${NC} $1"
}

# 打印失败
fail() {
    echo -e "  ${RED}[FAIL]${NC} $1"
}

# 打印信息
info() {
    echo -e "  ${BOLD}[INFO]${NC} $1"
}

# 带检查的编译阶段
# build_phase "name" fatal|warn <function_or_command>
build_phase() {
    local name="$1"
    local severity="$2"
    shift 2

    phase "$name"
    if "$@"; then
        ok "$name"
        return 0
    else
        if [ "$severity" = "fatal" ]; then
            fail "$name — 无法继续"
            exit 1
        else
            warn "$name — 跳过 (非致命)"
            return 1
        fi
    fi
}

# ── 清理 ──────────────────────────────────────────────────────────────────────

if $CLEAN_FIRST; then
    info "清理构建产物..."
    rm -rf "$OUTPUT_DIR" "$BUILD_DIR"
    ok "清理完毕"
    exit 0
fi

# ── Phase 0: 前置检查 ─────────────────────────────────────────────────────────

phase "前置检查"

# 检查 Rust 环境
if command -v rustc &>/dev/null; then
    ok "rustc $(rustc --version | awk '{print $2}')"
else
    warn "rustc 未安装，将尝试在 orbstack 中使用"
fi

# 检查 orbstack 或直接访问
if command -v orb &>/dev/null && orb status &>/dev/null 2>&1; then
    HAS_ORB=true
    ok "orbstack 运行中"
else
    HAS_ORB=false
fi

# 检查工具链
TOOLCHAIN_OK=false
if $HAS_ORB; then
    if _orb_test -f "$RISCV64_GCC"; then
        TOOLCHAIN_OK=true
        GCC_VER=$(_orb "$RISCV64_GCC" --version 2>&1 | head -1 || echo "unknown")
        ok "交叉工具链: $GCC_VER"
    fi
else
    if [ -f "$RISCV64_GCC" ]; then
        TOOLCHAIN_OK=true
        GCC_VER=$("$RISCV64_GCC" --version 2>&1 | head -1 || echo "unknown")
        ok "交叉工具链: $GCC_VER"
    fi
fi

if ! $TOOLCHAIN_OK; then
    fail "交叉工具链未找到 ($RISCV64_GCC)"
    fail "  交叉工具链是编译 camera-node 的必需条件"
    exit 1
fi

# 检查 OpenCV
OPENCV_OK=false
if $TOOLCHAIN_OK; then
    if $HAS_ORB; then
        if _orb_test -d "$RISCV64_OPENCV/include" && _orb_test -d "$RISCV64_OPENCV/lib"; then
            OPENCV_OK=true
            OPENCV_LIBS=$(_orb ls "$RISCV64_OPENCV/lib/" 2>/dev/null | tr '\n' ' ')
        fi
    else
        if [ -d "$RISCV64_OPENCV/include" ] && [ -d "$RISCV64_OPENCV/lib" ]; then
            OPENCV_OK=true
            OPENCV_LIBS=$(ls "$RISCV64_OPENCV/lib/" 2>/dev/null | tr '\n' ' ')
        fi
    fi
fi

if $OPENCV_OK; then
    ok "OpenCV: $RISCV64_OPENCV"
    info "  可用库: $OPENCV_LIBS"
else
    warn "OpenCV 未找到或不可用 ($RISCV64_OPENCV)"
    warn "  camera-node 已改用 V4L2 直接采集，无需 OpenCV"
fi

# 检查 Rust musl target
if command -v rustup &>/dev/null; then
    if rustup target list --installed 2>/dev/null | grep -q "$TARGET"; then
        ok "Rust target: $TARGET (已安装)"
    else
        info "正在安装 Rust target: $TARGET ..."
        rustup target add "$TARGET" 2>&1 | tail -1
        ok "Rust target: $TARGET (已安装)"
    fi
fi

# 检查 cargo
if command -v cargo &>/dev/null; then
    ok "cargo $(cargo --version | awk '{print $2}')"
fi

# 检查 git (用于克隆 dora-rs)
if command -v git &>/dev/null; then
    ok "git $(git --version | awk '{print $3}')"
elif $HAS_ORB && _orb which git &>/dev/null 2>&1; then
    ok "git (orbstack)"
else
    fail "git 未安装"
    exit 1
fi

# ── 打印构建计划 ──────────────────────────────────────────────────────────────

echo ""
echo -e "${BOLD}构建计划:${NC}"
echo "  目标架构:     $TARGET"
echo "  工具链:       $RISCV64_TOOLCHAIN"
echo "  dora-rs 版本: $DORA_REF"
echo "  OpenCV:       $RISCV64_OPENCV"
if $SKIP_CAMERA; then
    echo "  camera-node:  ${YELLOW}跳过${NC}"
fi
if $SKIP_DORA_RUNTIME; then
    echo "  dora 运行时:  ${YELLOW}跳过${NC}"
fi
echo "  输出目录:     $OUTPUT_DIR"
echo ""

# ── 准备输出目录 ──────────────────────────────────────────────────────────────

rm -rf "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR/bin"
mkdir -p "$PACKAGE_DIR/lib"
mkdir -p "$PACKAGE_DIR/etc"
mkdir -p "$BUILD_DIR"

# ── Phase 1: 交叉编译 dora 运行时 ─────────────────────────────────────────────

cross_compile_dora_runtime() {
    if $SKIP_DORA_RUNTIME; then
        skip "dora 运行时 (--skip-dora)"
        return 0
    fi

    if ! $HAS_ORB; then
        warn "orbstack 不可用，跳过 dora 运行时编译"
        warn "  需要 orbstack 访问 Xuantie 工具链和 sysroot"
        return 0
    fi

    local dora_src="$BUILD_DIR/dora-rs"

    # 克隆 (如果还没缓存)
    if [ ! -d "$dora_src/.git" ]; then
        info "克隆 dora-rs @ $DORA_REF ..."
        rm -rf "$dora_src"
        git clone --depth 1 --branch "$DORA_REF" \
            https://github.com/dora-rs/dora.git "$dora_src" 2>&1 | sed 's/^/    /'
    else
        info "使用已缓存的 dora-rs 源码: $dora_src"
    fi

    # Fix: 强制 Zenoh IPv4 监听 (SG2002 内核无 IPv6)
    _orb python3 -c "
src = open('$dora_src/libraries/core/src/topics.rs', 'r').read()
if 'SG2002: 强制 IPv4' not in src:
    src = src.replace(
        'let mut zenoh_config = zenoh::Config::default();',
        'let mut zenoh_config = zenoh::Config::default();\n'
        '            // SG2002: 强制 IPv4（内核无 IPv6）\n'
        '            zenoh_config.insert_json5(\"listen/endpoints\", r#\"[\"tcp/0.0.0.0:0\"]\"#).ok();'
    )
    src = src.replace('tcp/[::]', 'tcp/0.0.0.0')
    open('$dora_src/libraries/core/src/topics.rs', 'w').write(src)
    print('  [patch] OK: [::] -> 0.0.0.0 + listen/endpoints')
else:
    print('  [patch] SKIP: 已 patch 过')
" 2>&1

    # 配置交叉编译: rust-lld linker + Xuantie sysroot lib 路径 (供 libdl 等)
    local sysroot_lib="/home/junbo_dai/riscv64-linux-musl-x86_64/sysroot/usr/lib64v0p7_xthead/lp64d"
    local sysroot_lib2="/home/junbo_dai/riscv64-linux-musl-x86_64/sysroot/lib64v0p7_xthead/lp64d"

    _orb bash -c "
        . \"\$HOME/.cargo/env\"
        export CC_riscv64gc_unknown_linux_musl='$RISCV64_GCC'
        export CFLAGS_riscv64gc_unknown_linux_musl='-mcpu=c906fdv -mabi=lp64d'
        cd '$dora_src'
        mkdir -p .cargo
        cat > .cargo/config.toml << 'CFGEOF'
[target.$TARGET]
linker = \"rust-lld\"
rustflags = [
    \"-C\", \"target-feature=+crt-static\",
    \"-C\", \"link-arg=-L$sysroot_lib\",
    \"-C\", \"link-arg=-L$sysroot_lib2\",
]
CFGEOF
        cargo build -p dora-cli --target '$TARGET' --release --no-default-features 2>&1 | \
            grep -E 'Compiling|Finished|error' | sed 's/^/    /'
    " 2>&1

    # 收集产物
    local target_dir="$dora_src/target/$TARGET/release"
    if [ -f "$target_dir/dora" ]; then
        # Strip + copy
        if $TOOLCHAIN_OK; then
            _orb "$RISCV64_TOOLCHAIN/bin/riscv64-unknown-linux-musl-strip" \
                "$target_dir/dora" -o "$PACKAGE_DIR/bin/dora" 2>/dev/null || \
                cp "$target_dir/dora" "$PACKAGE_DIR/bin/dora"
        else
            cp "$target_dir/dora" "$PACKAGE_DIR/bin/dora"
        fi
        ok "dora 运行时编译完成 (coordinator + daemon + CLI)"
    else
        warn "dora-cli 编译失败"
        warn "  常见原因: 网络问题或 C 依赖编译失败"
        warn "  板子上可通过 curl 安装: curl -sSf https://dora-rs.ai/install.sh | sh"
        return 1
    fi

    return 0
}

build_phase "dora 运行时" "warn" cross_compile_dora_runtime

# ── Phase 2: 交叉编译 Rust 项目节点 ───────────────────────────────────────────

cross_compile_rust_nodes() {
    cd "$SCRIPT_DIR"

    # 配置交叉编译
    # 策略: 使用 rust-lld 作为 linker (兼容现代 ISA 字符串)，
    #       同时设置 Xuantie GCC 作为 C 编译器供 cc-rs 使用
    mkdir -p .cargo
    cat > .cargo/config.toml << CARGO_EOF
# 交叉编译配置 — 由 build_release.sh 生成
# 目标: SG2002 / CV1812 (riscv64 musl, Xuantie C906)

[target.$TARGET]
linker = "rust-lld"
rustflags = ["-C", "target-feature=+crt-static"]

[target.x86_64-apple-darwin]
# 保持默认 (host)
CARGO_EOF
    info "linker: rust-lld (内置)"

    # 设置 C 编译器环境变量 (供 cc-rs 编译 C 依赖如 ring, serialport)
    local cc_env=""
    if $TOOLCHAIN_OK; then
        export CC_riscv64gc_unknown_linux_musl="$RISCV64_GCC"
        export CFLAGS_riscv64gc_unknown_linux_musl="-mcpu=c906fdv -mabi=lp64d"
        cc_env="CC_riscv64gc_unknown_linux_musl=$RISCV64_GCC"
        info "C compiler: $RISCV64_GCC (Xuantie GCC 10.2)"
    else
        warn "无 Xuantie 工具链 — 有 C 依赖的 crate 可能编译失败"
    fi

    info "编译 Rust 节点 (web-server, motor-bridge, state-node, dora-c-ffi)..."

    # 在 orbstack 中编译 (使用 orbstack 中的 Rust 工具链和 Xuantie GCC)
    if $HAS_ORB; then
        _orb bash -c "
            . \"\$HOME/.cargo/env\"
            export CC_riscv64gc_unknown_linux_musl='$RISCV64_GCC'
            export CFLAGS_riscv64gc_unknown_linux_musl='-mcpu=c906fdv -mabi=lp64d'
            cd '$SCRIPT_DIR'
            cargo build \
                --target '$TARGET' \
                --release \
                -p web-server \
                -p motor-bridge \
                -p state-node \
                -p dora-c-ffi \
                2>&1 | grep -E 'Compiling|Finished|error' | sed 's/^/    /'
        " 2>&1
    else
        cargo build \
            --target "$TARGET" \
            --release \
            -p web-server \
            -p motor-bridge \
            -p state-node \
            -p dora-c-ffi \
            2>&1 | grep -E "Compiling|Finished|error|warning:" | sed 's/^/    /'
    fi

    # 检查产物
    local target_dir="target/$TARGET/release"
    local all_ok=true

    for bin in web-server motor-bridge state-node; do
        if [ -f "$target_dir/$bin" ]; then
            cp "$target_dir/$bin" "$PACKAGE_DIR/bin/"
            ok "$bin"
        else
            fail "$bin 编译失败"
            all_ok=false
        fi
    done

    # dora-c-ffi 是 staticlib
    if [ -f "$target_dir/libdora_c_ffi.a" ]; then
        cp "$target_dir/libdora_c_ffi.a" "$PACKAGE_DIR/lib/"
        ok "libdora_c_ffi.a"
    else
        warn "libdora_c_ffi.a 未找到 (camera-node 将无法编译)"
    fi

    # Strip 二进制瘦身
    if $TOOLCHAIN_OK; then
        info "Stripping binaries..."
        for bin in web-server motor-bridge state-node; do
            _orb "$RISCV64_TOOLCHAIN/bin/riscv64-unknown-linux-musl-strip" \
                "$PACKAGE_DIR/bin/$bin" 2>/dev/null || true
        done
    fi

    # libdora_c_ffi.a 保留在 PACKAGE_DIR/lib 中，Phase 3 (camera-node) 编译时需要
    cd "$SCRIPT_DIR"
    $all_ok || return 1
}

build_phase "Rust 项目节点" "fatal" cross_compile_rust_nodes

# ── Phase 3: 交叉编译 camera-node (C++) ───────────────────────────────────────

cross_compile_camera_node() {
    if $SKIP_CAMERA; then
        skip "camera-node (--skip-camera)"
        return 0
    fi

    local camera_dir="$SCRIPT_DIR/camera-node"
    local dora_ffi_lib="$PACKAGE_DIR/lib/libdora_c_ffi.a"

    if [ ! -f "$dora_ffi_lib" ]; then
        fail "libdora_c_ffi.a 未找到 ($dora_ffi_lib)"
        fail "  请确保 Phase 2 (Rust 项目节点) 编译成功"
        return 1
    fi

    info "编译 camera-node (V4L2, 无 OpenCV 依赖)..."

    # camera-node 直接用 V4L2 采集 MJPEG，不需要 OpenCV
    # 参考: tests/bench_camera_v4l2.cpp
    #
    # 关键: libdora_c_ffi.a 由 Rust (LLVM 19) 编译，包含了现代 ISA 扩展
    # (如 zifencei)，而 Xuantie GCC 10.2 的 ld 只认识旧的扩展名。
    # 修复: 用 objcopy 清除 .riscv.attributes 段，消掉 ISA 版本冲突。

    local camera_fixed_ffi="$BUILD_DIR/libdora_c_ffi_fixed.a"
    local objcopy_bin="$RISCV64_TOOLCHAIN/bin/riscv64-unknown-linux-musl-objcopy"

    info "修复 libdora_c_ffi.a (清除 .riscv.attributes)..."
    if $HAS_ORB; then
        _orb bash -c "
            set -e
            tmp=\$(mktemp -d)
            cd \"\$tmp\"
            ar x '$dora_ffi_lib'
            for f in *.o; do
                '$objcopy_bin' --remove-section=.riscv.attributes \"\$f\" 2>/dev/null || true
            done
            ar rcs '$camera_fixed_ffi' *.o
            rm -rf \"\$tmp\"
        " 2>&1
    else
        tmp=$(mktemp -d)
        cd "$tmp"
        ar x "$dora_ffi_lib"
        for f in *.o; do
            "$objcopy_bin" --remove-section=.riscv.attributes "$f" 2>/dev/null || true
        done
        ar rcs "$camera_fixed_ffi" *.o
        rm -rf "$tmp"
    fi
    ok "已生成兼容版本: $camera_fixed_ffi"

    # 生成交叉编译 Makefile
    cat > "$BUILD_DIR/Makefile.camera" << MAKEFILE_EOF
# 交叉编译 camera-node — 由 build_release.sh 生成
# 目标: riscv64 musl, SG2002/CV1812
# 源文件: main.cc + camera_v4l2.cpp（V4L2，无 OpenCV）

CXX      := $RISCV64_GXX
CXXFLAGS := -std=c++17 -O3 -Wall -mcpu=c906fdv -mabi=lp64d
LDFLAGS  := -static

TARGET   := $PACKAGE_DIR/bin/camera-node
SRCS     := $camera_dir/main.cc $camera_dir/camera_v4l2.cpp
OBJS     := $BUILD_DIR/camera-main.o $BUILD_DIR/camera-v4l2.o

DORA_FFI_LIB := $camera_fixed_ffi
INCLUDES := -I$camera_dir

.PHONY: all clean

all: \$(TARGET)

$BUILD_DIR/camera-main.o: $camera_dir/main.cc $camera_dir/camera.h
	@printf '  %b[CC]%b  main.cc\n' '\033[0;36m' '\033[0m'
	@\$(CXX) \$(CXXFLAGS) \$(INCLUDES) -c -o \$@ $camera_dir/main.cc

$BUILD_DIR/camera-v4l2.o: $camera_dir/camera_v4l2.cpp $camera_dir/camera.h
	@printf '  %b[CC]%b  camera_v4l2.cpp\n' '\033[0;36m' '\033[0m'
	@\$(CXX) \$(CXXFLAGS) \$(INCLUDES) -c -o \$@ $camera_dir/camera_v4l2.cpp

\$(TARGET): \$(OBJS) \$(DORA_FFI_LIB)
	@printf '  %b[LD]%b  camera-node\n' '\033[0;36m' '\033[0m'
	@\$(CXX) \$(CXXFLAGS) \$(LDFLAGS) -o \$@ \$(OBJS) \$(DORA_FFI_LIB) -lpthread

clean:
	rm -f \$(TARGET) \$(OBJS)
MAKEFILE_EOF

    if $HAS_ORB; then
        if _orb bash -c "
            export PATH=\"$RISCV64_TOOLCHAIN/bin:\$PATH\"
            make -f \"$BUILD_DIR/Makefile.camera\" 2>&1
        "; then
            ok "camera-node 编译完成"
        else
            fail "camera-node 编译失败"
            return 1
        fi
    else
        export PATH="$RISCV64_TOOLCHAIN/bin:$PATH"
        if make -f "$BUILD_DIR/Makefile.camera" 2>&1 | sed 's/^/    /'; then
            ok "camera-node 编译完成"
        else
            fail "camera-node 编译失败"
            return 1
        fi
    fi

    # 清理临时 .a
    rm -f "$camera_fixed_ffi"

    # 编译完成后清理 .a 文件 (运行时不需要)
    rm -f "$PACKAGE_DIR/lib/libdora_c_ffi.a" 2>/dev/null || true
    rmdir "$PACKAGE_DIR/lib" 2>/dev/null || true

    return 0
}

build_phase "camera-node (C++)" "fatal" cross_compile_camera_node

# ── Phase 4: 生成板子适配文件 ─────────────────────────────────────────────────

phase "生成板子适配文件"

# 复制配置文件
info "复制配置文件..."
cp "$SCRIPT_DIR/config.toml" "$PACKAGE_DIR/etc/"
ok "etc/config.toml"

# 复制前端静态文件 (web-server 从 ./static/ 读取)
info "复制静态文件..."
cp -r "$SCRIPT_DIR/web-server/static" "$PACKAGE_DIR/"
ok "static/"

# 生成 dataflow.yml (去掉 build: 字段，使用 bin/ 路径)
# 注意: camera-node 如果没有编译则省略
if [ -f "$PACKAGE_DIR/bin/camera-node" ]; then
    CAMERA_NODE="  - id: camera
    path: bin/camera-node
    inputs:
      tick: dora/timer/millis/33
      control: web-server/control
    outputs:
      - image"
    WEB_IMAGE_INPUT="image: camera/image"
else
    CAMERA_NODE=""
    WEB_IMAGE_INPUT=""
fi

cat > "$PACKAGE_DIR/etc/dataflow.yml" << EOF
# dora 数据流 — SG2002 板子部署版
# 由 build_release.sh 自动生成

nodes:
${CAMERA_NODE}
  - id: motor-bridge
    path: bin/motor-bridge
    inputs:
      motor_cmd: web-server/motor_cmd
    outputs:
      - motor_status

  - id: state-node
    path: bin/state-node
    inputs:
      motor_status: motor-bridge/motor_status
    outputs:
      - robot_state

  - id: web-server
    path: bin/web-server
    inputs:
${WEB_IMAGE_INPUT:+      image: camera/image}
      robot_state: state-node/robot_state
    outputs:
      - control
      - motor_cmd
EOF
ok "etc/dataflow.yml (板子适配版)"

# 生成 init.sh
cat > "$PACKAGE_DIR/init.sh" << 'INITEOF'
#!/bin/sh
# =============================================================================
# dora 机器人系统 — SG2002 板子启动脚本
#
# 用法:
#   cd /root/dora-riscv64 && ./init.sh
#   DORA_HOME=/opt/dora ./init.sh     # 自定义路径
#
# 停止: ./stop.sh
# =============================================================================

set -e

DORA_HOME="${DORA_HOME:-$(cd "$(dirname "$0")" && pwd)}"
cd "$DORA_HOME"

echo "========================================"
echo "  dora robot system (riscv64)"
echo "========================================"

# 读取配置
BACKEND=$(grep 'backend' etc/config.toml 2>/dev/null | head -1 | cut -d'"' -f2)
echo "  motor backend: ${BACKEND:-dev}"

DORA_BIN="$DORA_HOME/bin/dora"

# ── 1. 验证二进制文件 ──
echo ""
echo "Verifying binaries..."
MISSING=""
for bin in web-server motor-bridge state-node; do
    if [ -f "$DORA_HOME/bin/$bin" ]; then
        echo "  OK  bin/$bin"
    else
        echo "  MISS bin/$bin"
        MISSING="$MISSING $bin"
    fi
done

# camera-node 可选
if [ -f "$DORA_HOME/bin/camera-node" ]; then
    echo "  OK  bin/camera-node"
else
    echo "  --  bin/camera-node (not included, skipping)"
fi

if [ -n "$MISSING" ]; then
    echo ""
    echo "ERROR: Missing binaries:$MISSING"
    echo "  Please run './build_release.sh' on your dev machine first."
    exit 1
fi

# ── 2. 设置库路径 (如果有 OpenCV .so) ──
if [ -d "$DORA_HOME/lib" ]; then
    export LD_LIBRARY_PATH="$DORA_HOME/lib:$DORA_HOME/lib/opencv:${LD_LIBRARY_PATH:-}"
fi

# ── 3. 启动 dora 运行时 ──
echo ""
echo "Starting dora runtime..."

if [ -f "$DORA_BIN" ]; then
    echo "  Using bundled dora binary"
    # 使用内置的 dora 二进制
    "$DORA_BIN" up 2>/dev/null &
    DORA_UP_PID=$!
    sleep 2
else
    echo "  Using system dora (make sure it's installed)"
    dora up 2>/dev/null &
    DORA_UP_PID=$!
    sleep 2
fi

# ── 4. 启动数据流 ──
echo ""
echo "Launching dataflow..."

if [ -f "$DORA_BIN" ]; then
    "$DORA_BIN" run "$DORA_HOME/etc/dataflow.yml" &
    DORA_RUN_PID=$!
else
    dora run "$DORA_HOME/etc/dataflow.yml" &
    DORA_RUN_PID=$!
fi

# ── 5. 等待就绪 ──
echo ""
echo "Waiting for web-server..."
WEB_PORT=$(grep 'port' "$DORA_HOME/etc/config.toml" 2>/dev/null | grep -oE '[0-9]+' | head -1)
WEB_PORT="${WEB_PORT:-80}"

for i in $(seq 1 60); do
    sleep 1
    if curl -s "http://localhost:${WEB_PORT}/api/camera/status" > /dev/null 2>&1; then
        echo ""
        echo "========================================"
        echo "  READY!"
        echo ""
        echo "  Web UI: http://<board-ip>:${WEB_PORT}"
        echo ""
        echo "  Stop:   cd $DORA_HOME && ./stop.sh"
        echo "  Logs:   cd $DORA_HOME && ./bin/dora logs"
        echo "========================================"
        exit 0
    fi
    printf "."
done

echo ""
echo "========================================"
echo "  WARNING: web-server 未响应"
echo ""
echo "  Check with: cd $DORA_HOME && ./bin/dora logs"
echo "========================================"
exit 1
INITEOF
chmod +x "$PACKAGE_DIR/init.sh"
ok "init.sh"

# 生成 stop.sh
cat > "$PACKAGE_DIR/stop.sh" << 'STOPEOF'
#!/bin/sh
# =============================================================================
# dora 机器人系统 — 停止脚本
# =============================================================================

set -e

DORA_HOME="${DORA_HOME:-$(cd "$(dirname "$0")" && pwd)}"
DORA_BIN="$DORA_HOME/bin/dora"

echo "Stopping dora system..."

# 使用内置 dora 或系统 dora
if [ -f "$DORA_BIN" ]; then
    "$DORA_BIN" stop 2>/dev/null || true
    "$DORA_BIN" destroy 2>/dev/null || true
else
    dora stop 2>/dev/null || true
    dora destroy 2>/dev/null || true
fi

# 兜底: 杀掉所有相关进程
for name in camera-node web-server motor-bridge state-node dora-daemon dora-coordinator; do
    pgrep -f "$name" 2>/dev/null | while read pid; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
done

sleep 0.3

# 强制杀死
for name in camera-node web-server motor-bridge state-node dora-daemon dora-coordinator; do
    pgrep -f "$name" 2>/dev/null | while read pid; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null
    done
done

# 释放端口
WEB_PORT=$(grep 'port' "$DORA_HOME/etc/config.toml" 2>/dev/null | grep -oE '[0-9]+' | head -1)
WEB_PORT="${WEB_PORT:-80}"
fuser -k "${WEB_PORT}/tcp" 2>/dev/null || true

echo "Done"
STOPEOF
chmod +x "$PACKAGE_DIR/stop.sh"
ok "stop.sh"

# 生成 README.md
cat > "$PACKAGE_DIR/README.md" << 'READMEEOF'
# dora 机器人系统 (riscv64)

SG2002 / CV1812 板子部署包。

## 系统要求

- SG2002 控制板 (LicheeRV Nano / Milk-V Duo 等)
- Linux (Buildroot / StarryOS)
- RISC-V 64 架构
- 至少 100MB 可用空间

## 快速开始

```bash
# 1. 解压
cd /root
tar xzf dora-riscv64.tar.gz
cd dora-riscv64

# 2. 配置 (如需修改)
vi etc/config.toml
# 将 motor.backend 从 "dev" 改为 "tt_pid" (真实硬件)
# 将 arm.backend 从 "dev" 改为 "zp10s" (真实硬件)

# 3. 启动
./init.sh

# 4. 停止
./stop.sh
```

## 配置说明

`etc/config.toml`:

| 节点 | 选项 | 说明 |
|------|------|------|
| `motor.backend` | `dev` / `tt_pid` | 电机驱动: 开发模拟 / 真实 TT PID |
| `motor.port` | `/dev/ttyS1` | 电机串口 |
| `arm.backend` | `dev` / `zp10s` | 机械臂驱动: 开发模拟 / 真实 ZP10S 舵机 |
| `arm.port` | `/dev/ttyS2` | 机械臂串口 |
| `web.port` | `80` | HTTP 服务端口 |

## 开机自启

```bash
# 添加启动脚本 (Buildroot init.d 方式)
cat > /etc/init.d/S99dora << 'EOF'
#!/bin/sh
case "$1" in
  start)
    echo "Starting dora..."
    cd /root/dora-riscv64 && ./init.sh &
    ;;
  stop)
    cd /root/dora-riscv64 && ./stop.sh
    ;;
  *)
    echo "Usage: $0 {start|stop}"
    ;;
esac
EOF

chmod +x /etc/init.d/S99dora
```

## 故障排查

```bash
# 查看日志
./bin/dora logs

# 手动测试 web-server
curl http://localhost/api/camera/status

# 检查串口是否就绪
ls -la /dev/ttyS1 /dev/ttyS2

# 确保 UART pinmux 已配置 (SG2002)
devmem 0x03001070 32 0x2   # UART2 TX
devmem 0x03001074 32 0x2   # UART2 RX
```
READMEEOF
ok "README.md"

# ── Phase 5: 打包 ─────────────────────────────────────────────────────────────

phase "打包"

cd "$OUTPUT_DIR"

# 检查 bin 目录是否有必要的二进制
BIN_COUNT=$(ls "$PACKAGE_DIR/bin/" 2>/dev/null | wc -l | tr -d ' ')
info "bin/ 包含 $BIN_COUNT 个文件:"
ls -la "$PACKAGE_DIR/bin/" 2>/dev/null | sed 's/^/    /'

# 显示文件类型 (如果在 macOS 上用 file 检查交叉编译的二进制可能不准)
if [ -f "$PACKAGE_DIR/bin/web-server" ]; then
    WEB_SIZE=$(du -h "$PACKAGE_DIR/bin/web-server" | cut -f1)
    info "web-server 大小: $WEB_SIZE"
fi

# 打包
TARBALL="$OUTPUT_DIR/${PACKAGE_NAME}.tar.gz"
info "创建 $TARBALL ..."
COPYFILE_DISABLE=1 tar czf "$TARBALL" "$PACKAGE_NAME"

TARBALL_SIZE=$(du -h "$TARBALL" | cut -f1)
ok "打包完成: $TARBALL ($TARBALL_SIZE)"

# ── Phase 6: 部署 (可选) ───────────────────────────────────────────────────────

if [ -n "$DEPLOY_HOST" ]; then
    phase "部署到 $DEPLOY_HOST"

    info "上传 tarball..."
    scp "$TARBALL" "root@${DEPLOY_HOST}:/root/" || {
        fail "scp 失败"
        exit 1
    }
    ok "上传完成"

    info "解压并初始化..."
    ssh "root@${DEPLOY_HOST}" << ENDSSH
        set -e
        cd /root

        # 停止已有服务
        if [ -d dora-riscv64 ]; then
            cd dora-riscv64 && ./stop.sh 2>/dev/null || true
            cd /root
        fi

        # 解压新版本
        rm -rf dora-riscv64.old
        [ -d dora-riscv64 ] && mv dora-riscv64 dora-riscv64.old
        tar xzf dora-riscv64.tar.gz
        cd dora-riscv64
        chmod +x bin/* init.sh stop.sh 2>/dev/null || true

        # 启动
        ./init.sh
ENDSSH

    ok "部署完成! 访问 http://${DEPLOY_HOST}/"
fi

# ── 总结 ──────────────────────────────────────────────────────────────────────

echo ""
echo -e "${GREEN}${BOLD}══════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}${BOLD}  构建完成${NC}"
echo -e "${GREEN}${BOLD}══════════════════════════════════════════════════════════${NC}"
echo ""
echo "  产物: $TARBALL ($TARBALL_SIZE)"
echo "  内容:"
echo "    bin/"
ls -1 "$PACKAGE_DIR/bin/" 2>/dev/null | sed 's/^/      /'
echo "    etc/"
ls -1 "$PACKAGE_DIR/etc/" 2>/dev/null | sed 's/^/      /'
echo "    *.sh"
ls -1 "$PACKAGE_DIR/"*.sh 2>/dev/null | sed 's|.*/||; s/^/      /'
echo ""
echo "  部署:"
echo "    scp $TARBALL root@<robot>:/root/"
echo "    ssh root@<robot> 'cd /root && tar xzf ${PACKAGE_NAME}.tar.gz && cd ${PACKAGE_NAME} && ./init.sh'"
echo ""
if $SKIP_CAMERA; then
    echo -e "  ${YELLOW}注意: camera-node 未编译 (--skip-camera 或无工具链)${NC}"
    echo "    如需摄像头支持:"
    echo "    1. 确保 Xuantie 工具链可用: $RISCV64_TOOLCHAIN"
    echo "    2. 确保 OpenCV 静态库可用: $RISCV64_OPENCV/lib/libopencv_*.a"
    echo "    3. 去掉 --skip-camera 重新编译"
fi
echo ""
