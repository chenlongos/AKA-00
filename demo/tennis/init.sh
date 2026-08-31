#!/bin/sh
# tennis demo 启动脚本
#
# 路径无关：从 init.sh 自身位置反推应用根目录。
#   demo/tennis/init.sh -> dirname=`demo/tennis` -> ..=应用根
# 因此无论部署目录叫 `AKA-00` 还是别的，从 /root/$(anything)/demo/tennis/init.sh
# 启动，$APP_DIR 都会自动设对。
#
# .so 加载路径硬性约束在应用根下的 libs/ 子目录：
#   libcviruntime.so / libcvikernel.so / libopencv_*.so.3.2
# 由打包脚本一并部署（cpp/dist 的 libs/）。
# 找不到时 musl loader 会逐条打印缺哪个 lib + symbol，便于排查。

# 路径无关：先把 $0 解析成绝对路径（POSIX 写法，不依赖 readlink -f），
# 再从 init.sh 位置反推应用根 (demo/tennis/init.sh -> ../../)。
INIT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
APP_DIR="$(cd "$INIT_DIR/../.." && pwd)"

export LD_LIBRARY_PATH="$APP_DIR/libs:${LD_LIBRARY_PATH:-}"

exec ./tennis ./yolo_model.cvimodel 0
