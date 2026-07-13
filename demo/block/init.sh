#!/bin/sh
# demo-node 用独立进程组 spawn 这个 init.sh，当前 cwd = demo/<name>/。
# 路径无关：从 init.sh 自身位置反推仓库根，无论部署在 /root/AKA-00 还是别的目录。
#   demo/<name>/init.sh -> dirname=`demo/<name>` -> ../..=repo 根
#
# .so 加载路径在仓库根下的 libs/ 子目录，由用户单独 scp 上来（不打包进 tarball）。
# musl loader 找不到时逐条报缺哪个 lib + symbol。

# 路径无关：先把 $0 解析成绝对路径，再反推仓库根
INIT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
DORA_HOME="$(cd "$INIT_DIR/../.." && pwd)"

export LD_LIBRARY_PATH="$DORA_HOME/libs:${LD_LIBRARY_PATH:-}"

exec ./tennis ./yolo_model.cvimodel 0
