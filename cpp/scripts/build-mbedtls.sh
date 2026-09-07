#!/bin/sh
# =============================================================================
# 交叉编译 mbedTLS（riscv64 musl 静态）→ cpp/third_party/mbedtls/
#
# capp 的 HTTPS 服务（HttpServer::listen_tls）走 mbedTLS 做 TLS 终止。
# 系统不再自带交叉版 mbedTLS，首次交叉编译前先跑一次本脚本。
#
# 用法:
#   ./build-mbedtls.sh                            # 自动 git clone mbedtls 并交叉编译
#   ./build-mbedtls.sh /path/to/mbedtls-src       # 用本地源码目录（已 git submodule init）
#   TOOLCHAIN_PREFIX=/path/to/riscv64-unknown-linux-musl- ./build-mbedtls.sh
#
# 产物: cpp/third_party/mbedtls/{libmbedtls.a, libmbedcrypto.a, libmbedx509.a}
#
# 说明：mbedTLS 3.x 用 CMake 构建（已移除 ./configure）；其 framework/ 子模块必须
# 初始化，所以这里用 git clone 而不是下载 release tarball（release tarball 不含
# 子模块）。
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../third_party/mbedtls"
WORK="$HERE/../third_party/build-mbedtls"
VERSION="3.6.7"

TOOLCHAIN_PREFIX="${TOOLCHAIN_PREFIX:-/home/junbo_dai/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-}"
CC="${TOOLCHAIN_PREFIX}gcc"
AR="${TOOLCHAIN_PREFIX}ar"

# 源：优先命令行参数（本地目录），否则 git clone
SRC_DIR="$WORK/src"
if [ -n "${1:-}" ] && [ -d "$1" ]; then
    SRC_DIR="$1"
fi
if [ ! -d "$SRC_DIR/framework" ] || [ ! -d "$SRC_DIR/library" ]; then
    echo "[mbedtls] cloning v${VERSION} (with framework submodule)..."
    mkdir -p "$WORK"
    rm -rf "$SRC_DIR"
    git clone --depth 1 --branch "v${VERSION}" \
        https://github.com/Mbed-TLS/mbedtls.git "$SRC_DIR" >/dev/null
    ( cd "$SRC_DIR" && git submodule update --init --depth 1 >/dev/null )
fi

mkdir -p "$OUT"
rm -rf "$WORK/build"
mkdir -p "$WORK/build"
cd "$WORK/build"

echo "[mbedtls] configuring for riscv64 musl (static)..."
cmake "$SRC_DIR" \
    -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_AR="$AR" \
    -DCMAKE_C_FLAGS="-O2 -mcpu=c906fdv -mabi=lp64d" \
    -DENABLE_TESTING=Off \
    -DENABLE_PROGRAMS=Off \
    -DMBEDTLS_FATAL_WARNINGS=Off \
    >/dev/null

echo "[mbedtls] building..."
make -j"$(nproc)" >/dev/null
make install >/dev/null

echo "[mbedtls] ✓ $OUT/lib/libmbed{tls,crypto,x509}.a"
ls -la "$OUT/lib"/libmbed*.a