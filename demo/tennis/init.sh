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

# 模型：优选用 demo 目录里自带的（前端上传/下载模型会放这里），
# 否则用应用根下模型库里的同名模型 models/<demo 目录名>.cvimodel。
DEMO_NAME="$(basename "$INIT_DIR")"
MODEL="./yolo_model.cvimodel"
[ -f "$MODEL" ] || MODEL="$APP_DIR/models/$DEMO_NAME.cvimodel"
if [ ! -f "$MODEL" ]; then
    echo "[demo] 找不到模型：$MODEL" >&2
    echo "[demo] 把模型放到 $APP_DIR/models/$DEMO_NAME.cvimodel 再启动" >&2
    exit 1
fi

exec ./tennis "$MODEL" 0
