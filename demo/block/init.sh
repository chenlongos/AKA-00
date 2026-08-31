#!/bin/sh
# block demo 启动脚本（capp 用独立进程组 spawn 这个 init.sh，当前 cwd = demo/<name>/）。
# 路径无关：从 init.sh 自身位置反推应用根，无论部署在 /root/AKA-00 还是别的目录。
#   demo/<name>/init.sh -> dirname=`demo/<name>` -> ../..=应用根
#
# .so 加载路径在应用根下的 libs/ 子目录，由打包脚本一并部署（cpp/dist 的 libs/）。
# musl loader 找不到时逐条报缺哪个 lib + symbol。

# 路径无关：先把 $0 解析成绝对路径，再反推应用根
INIT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
APP_DIR="$(cd "$INIT_DIR/../.." && pwd)"

export LD_LIBRARY_PATH="$APP_DIR/libs:${LD_LIBRARY_PATH:-}"

exec ./tennis ./yolo_model.cvimodel 0
