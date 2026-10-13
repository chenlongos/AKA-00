#!/bin/sh
# =============================================================================
# 交叉编译 libjpeg（riscv64 musl 静态）→ cpp/third_party/jpeg/
#
# SG2002 上 capp 的摄像头需要 libjpeg（解码/编码）。系统不再自带交叉版 libjpeg，
# 首次交叉编译前先跑一次本脚本。
#
# 用法:
#   ./build-libjpeg.sh                        # 自动下载 jpegsrc.v9f 并交叉编译
#   ./build-libjpeg.sh /path/to/jpegsrc.tar.gz  # 用本地源码包
#   TOOLCHAIN_PREFIX=/path/to/riscv64-unknown-linux-musl- ./build-libjpeg.sh
#   AKA_TOOLCHAIN_DIR=/path/to/toolchain/ ./build-libjpeg.sh   # 取 <dir>bin/ 下的交叉前缀
#
# 产物: cpp/third_party/jpeg/{libjpeg.a, jpeglib.h, ...}
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../third_party/jpeg"
WORK="$HERE/../third_party/build-jpeg"

# 工具链不写死路径：优先 TOOLCHAIN_PREFIX，其次 AKA_TOOLCHAIN_DIR/bin/，最后回退 PATH 里的裸前缀
if [ -z "${TOOLCHAIN_PREFIX:-}" ]; then
    if [ -n "${AKA_TOOLCHAIN_DIR:-}" ]; then
        TOOLCHAIN_PREFIX="${AKA_TOOLCHAIN_DIR%/}/bin/riscv64-unknown-linux-musl-"
    else
        TOOLCHAIN_PREFIX="riscv64-unknown-linux-musl-"
    fi
fi
CC="${TOOLCHAIN_PREFIX}gcc"
AR="${TOOLCHAIN_PREFIX}ar"

# 源包：优先命令行参数 / 本地缓存，否则下载 ijg.org
TARBALL="${1:-}"
if [ -z "$TARBALL" ]; then
    for cand in \
        "$HOME/dl/jpegsrc.v9f.tar.gz" \
        "$HERE/../third_party/jpegsrc.v9f.tar.gz"; do
        if [ -f "$cand" ]; then TARBALL="$cand"; break; fi
    done
fi
if [ -z "$TARBALL" ]; then
    echo "[libjpeg] downloading jpegsrc.v9f.tar.gz ..."
    TARBALL="$HERE/../third_party/jpegsrc.v9f.tar.gz"
    mkdir -p "$HERE/../third_party"
    curl -fL -o "$TARBALL" https://ijg.org/files/jpegsrc.v9f.tar.gz
fi

mkdir -p "$WORK" "$OUT" "$HERE/../third_party"
rm -rf "$WORK/src"
mkdir -p "$WORK/src"
tar -xzf "$TARBALL" -C "$WORK/src" --strip-components=1

cd "$WORK/src"
echo "[libjpeg] configuring for riscv64 musl (static)..."
./configure --host=riscv64-unknown-linux-musl \
    --prefix="$OUT" \
    --disable-shared --enable-static \
    CC="$CC" AR="$AR" \
    CFLAGS="-O3 -mcpu=c906fdv -mabi=lp64d"

echo "[libjpeg] building..."
make -j"$(nproc)" >/dev/null
make install >/dev/null

echo "[libjpeg] ✓ $OUT/lib/libjpeg.a"
ls -la "$OUT/lib/libjpeg.a"
