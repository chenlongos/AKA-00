#!/bin/sh
# tennis demo 启动脚本 —— 运行仓库A（robot-controller）的自动捡球程序
#
# 仓库A 部署在固定路径 /home/cat/robot-controller，运行命令 python -m src.main，
# 依赖 venv /home/cat/venv_rk3576（rknn 运行时）。
#
# 通过环境变量注入配置：
#   AKA00_ARM_ANGLES -> 仓库B根目录的 arm_angles.json（Web 界面在线调好的那份），
#                       仓库A启动时据此生成动作序列
#   AKA00_MODEL      -> 仓库A自带的网球检测 rknn 模型

ROBOT_CONTROLLER="/home/cat/robot-controller"
VENV_ACTIVATE="/home/cat/venv_rk3576/bin/activate"

# 路径无关：从 init.sh 自身位置反推仓库B根目录（demo/tennis/init.sh -> ../../），
# 与部署目录名无关（AKA_HOME 默认 /home/cat/aka00）。
INIT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
AKA_HOME="$(cd "$INIT_DIR/../.." && pwd)"

if [ ! -f "$VENV_ACTIVATE" ]; then
    echo "tennis demo: venv 不存在: $VENV_ACTIVATE" >&2
    exit 1
fi
if [ ! -d "$ROBOT_CONTROLLER/src" ]; then
    echo "tennis demo: 仓库A不存在: $ROBOT_CONTROLLER" >&2
    exit 1
fi

. "$VENV_ACTIVATE"

export AKA00_ARM_ANGLES="$AKA_HOME/arm_angles.json"
export AKA00_MODEL="$ROBOT_CONTROLLER/models/yolov8n-int8-tennis.rknn"

# main.py 读 config/ 等相对路径，必须在仓库A根目录下运行
cd "$ROBOT_CONTROLLER" || exit 1

exec python -m src.main
