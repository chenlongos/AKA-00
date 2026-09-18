#!/bin/sh
# =============================================================================
# 交叉编译 Lua（riscv64 musl 静态）→ cpp/third_party/lua/
#
# capp 用它跑**动作脚本**（demo/*.lua，如 grab / approach）：把"追物体并抓起来"这类**要反复调参的流程**
# 从 C++ 里搬出来，改一行存盘重跑即可，不用交叉编译 + 部署 + 重启。
# 宿主只提供原语（取框/驱动/抓/等待），安全兜底（限速、抢占、内存预算、看门狗）留在 C++。
#
# Lua 是纯 C、无依赖、约 200KB，交叉编译就是"把 src/*.c 全编一遍"（除了带 main 的
# lua.c / luac.c）。
#
# 用法:
#   ./build-lua.sh                       # 自动下载 lua-5.4.7 并交叉编译
#   ./build-lua.sh /path/to/lua.tar.gz   # 用本地源码包
#   TOOLCHAIN_PREFIX=/path/to/riscv64-unknown-linux-musl- ./build-lua.sh
#   HOST=1 ./build-lua.sh                # 编本机版（开发机上跑 capp 调试用）
#
# 产物: cpp/third_party/lua/{lib/liblua.a, include/lua.h 等 4 个头}
# =============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../third_party/lua"
WORK="$HERE/../third_party/build-lua"
VER="lua-5.4.7"

TOOLCHAIN_PREFIX="${TOOLCHAIN_PREFIX:-/home/junbo_dai/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-}"
CC="${TOOLCHAIN_PREFIX}gcc"
AR="${TOOLCHAIN_PREFIX}ar"
CFLAGS="-O2 -DLUA_USE_POSIX"

if [ "${HOST:-0}" = "1" ]; then
    CC="${CC_OVERRIDE:-cc}"
    AR="${AR_OVERRIDE:-ar}"
    OUT="$HERE/../third_party/lua-host"
    echo "[lua] 本机版（开发机调试用）→ $OUT"
fi

TARBALL="${1:-}"
if [ -z "$TARBALL" ]; then
    for cand in "$HOME/dl/$VER.tar.gz" "$HERE/../third_party/$VER.tar.gz"; do
        [ -f "$cand" ] && { TARBALL="$cand"; break; }
    done
fi
if [ -z "$TARBALL" ]; then
    echo "[lua] downloading $VER.tar.gz ..."
    TARBALL="$HERE/../third_party/$VER.tar.gz"
    mkdir -p "$HERE/../third_party"
    curl -fL -o "$TARBALL" "https://www.lua.org/ftp/$VER.tar.gz"
fi

mkdir -p "$WORK" "$OUT/include" "$OUT/lib"
rm -rf "$WORK/src"
mkdir -p "$WORK/src"
tar -xzf "$TARBALL" -C "$WORK/src" --strip-components=1

cd "$WORK/src/src"
echo "[lua] building $(basename "$CC") ..."
rm -rf "$WORK/obj"
mkdir -p "$WORK/obj"
# 除了 lua.c / luac.c（它们带 main，是独立解释器/编译器）
for f in *.c; do
    case "$f" in lua.c|luac.c) continue ;; esac
    "$CC" $CFLAGS -c -o "$WORK/obj/${f%.c}.o" "$f"
done
"$AR" rcs "$OUT/lib/liblua.a" "$WORK"/obj/*.o

# 头文件（脚本宿主只需要 lua.h/lauxlib.h/lualib.h/luaconf.h）
cp lua.h luaconf.h lualib.h lauxlib.h "$OUT/include/"

echo "[lua] ✓ $OUT/lib/liblua.a"
ls -la "$OUT/lib/liblua.a"
