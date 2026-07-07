#!/bin/sh
# demo-node 用 process_group(0) 独立进程组 spawn 这个 init.sh，
# 当前 cwd = demo/<name>/。使用 $DORA_HOME/lib 找 CVitek + OpenCV 共享库，
# 由 build_release.sh 复制到 PACKAGE_DIR/lib。
# init.sh 里不写死 /root/AKA-00/lib 路径，方便换部署目录。

DORA_HOME="${DORA_HOME:-/root/dora-riscv64}"
export LD_LIBRARY_PATH="$DORA_HOME/lib:${LD_LIBRARY_PATH:-}"
exec ./tennis ./yolo_model.cvimodel 0