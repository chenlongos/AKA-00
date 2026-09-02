#!/bin/sh
# board_detect.sh — 读取设备树 model，归一化后写入 /run/board_name.cache
#
# 用法:
#   1) 早期初始化直接执行:        sh board_detect.sh          （缓存不存在则创建）
#   2) 其它脚本 source 后调用:     . ./board_detect.sh
#                                   b=$(get_board_name)        （缓存不存在则自动创建，再读缓存）

CACHE_FILE="/run/board_name.cache"
DT_MODEL="/proc/device-tree/model"

# 把设备树 model 归一化为板型名（与 app/config.py 中的键保持一致）
_normalize_model() {
    _model=""
    [ -f "$DT_MODEL" ] && _model=$(cat "$DT_MODEL" 2>/dev/null)
    case "$_model" in
        "EmbedFire LubanCat-3") echo "lubancat3" ;;
        "LicheeRv Nano")        echo "licheervnano" ;;
        *)                      echo "" ;;
    esac
}

# 写缓存（/run 需要 root；非 root 用户自动 sudo）
_write_cache() {
    _name=$(_normalize_model)
    [ -z "$_name" ] && { echo "[board_detect] device-tree model 未知，跳过写缓存"; return 1; }
    if [ "$(id -u)" -ne 0 ]; then
        sudo sh -c "printf '%s\n' '$_name' > '$CACHE_FILE'"
    else
        printf '%s\n' "$_name" > "$CACHE_FILE"
    fi
    echo "[board_detect] board_name=$_name -> $CACHE_FILE"
}

# 供 source 调用：返回缓存中的板型名（缓存不存在时返回空串）
get_board_name() {
    cat "$CACHE_FILE" 2>/dev/null
}

# 不论直接执行还是被 source：缓存已存在则跳过（不读设备树），不存在则创建
if [ ! -f "$CACHE_FILE" ]; then
    _write_cache
fi
