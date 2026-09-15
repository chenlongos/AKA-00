#!/bin/sh
# =============================================================================
# build-ota.sh — 生成 cpp 版的自解压安装器（首次部署 + OTA 升级共用）
#
# 产物:  cpp/dist/aka-00-server
#   ./aka-00-server              正常启动（首次自动解包 → init.sh）
#   ./aka-00-server --init       首次部署（解包 + AP/Web 初始化）
#   ./aka-00-server --update     OTA 升级（保配置换包 + 重启服务）
#   ./aka-00-server --extract    仅解包到 $AKA_HOME（不重启；验证/CI 用）
#
# 用法:  cd cpp && make ota      # 内部先 make package，再跑本脚本
#        AKA_HOME=/tmp/x ./dist/aka-00-server --extract   # 本地验证
#
# 为什么是"自解压单文件"而不是 tar.gz：
#   云端 /api/user/robot-versions/featured 只给一个 imageUrl，capp 下载后
#   直接 `exec <文件> --update`（routes.cpp 的 write_restart_script）。
#   因此发布物必须自带解包+安装逻辑 —— 与 Python 版 aka-00-server 的契约一致。
#
# 实现要点：
#   1) payload 是 tar.gz，直接追加在脚本头之后（不做 base64，不依赖 python3：
#      板上 busybox 的 tail/tar 就够）。
#   2) 头里写死 PLAYLOAD 字节偏移（定宽 7 位零填充，替换后长度不变，偏移才准）。
#   3) --update 先停 capp **和守护脚本 init.sh**（否则守护 2 秒后把旧 capp 拉起，
#      会和换包过程打架），换包后再 exec init.sh 重新拉起。
#   4) 换包用 staging + 目录改名（尽量原子），旧目录留成 .old 作为回滚点；
#      并默认保留用户运行时文件（config.toml / speed_config.json / arm_angles.json）。
# =============================================================================
set -e

CPP="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$CPP/dist"
SRC="$DIST/AKA-00"
OUT="$DIST/aka-00-server"

if [ ! -x "$SRC/aka-capp" ]; then
    echo "错误: 缺少 $SRC/aka-capp —— 先跑 'make package'" >&2
    exit 1
fi

VER="$(cut -d@ -f1 "$SRC/VERSION" 2>/dev/null || echo unknown)"
echo "── 生成自解压安装器（版本 $VER）──"

# ── 头部（单引号 heredoc：内部 $ 不做展开，留给目标机执行）──
# 先落在临时文件，payload 校验通过后再原子 mv 到 $OUT：
# 否则一旦中途失败（tar 报错/磁盘满），dist 里会留一个截断的半成品 —— 部署它必然炸。
TMP_OUT="$OUT.tmp.$$"
TMP_PAYLOAD="${TMPDIR:-/tmp}/aka-ota-payload.$$.tgz"
rm -f "$TMP_OUT" "$TMP_PAYLOAD"
trap 'rm -f "$TMP_OUT" "$TMP_PAYLOAD"' EXIT INT TERM

cat > "$TMP_OUT" <<'HEADER'
#!/bin/sh
# =============================================================================
# AKA-00 自解压安装器（cpp 版：aka-capp + csrc）
#
#   ./aka-00-server              正常启动（首次自动解包 → init.sh）
#   ./aka-00-server --init       首次部署（解包 + AP/Web 初始化）
#   ./aka-00-server --update     OTA 升级（保配置换包 + 重启服务）
#   ./aka-00-server --extract    仅解包到 $AKA_HOME（不重启）
#
# 环境变量：
#   AKA_HOME              部署目录（默认 /root/AKA-00）
#   AKA_OTA_RESET_CONFIG=1  升级时连 config.toml 一起覆盖（默认保留用户配置）
# =============================================================================
set -e

AKA_HOME="${AKA_HOME:-/root/AKA-00}"
# 定宽 7 位（构建脚本回填）：payload 在自身文件中的字节偏移（1-based）
PAYLOAD_OFFSET=0000000

# 用户运行时数据：升级默认保留（标定/限速/配置都是现场数据）
KEEP_FILES="config.toml speed_config.json arm_angles.json"

extract_payload() {
    _dest="$1"
    mkdir -p "$_dest"
    # 偏移是 7 位零填充（保证替换前后头长度不变），必须去掉前导零再用：
    # 直接 tail -c +0003534 会被当成八进制解析（=1884），解包位置就错了。
    _off="$(printf '%s' "$PAYLOAD_OFFSET" | sed 's/^0*//')"
    [ -n "$_off" ] || _off=0
    tail -c +"$_off" "$0" | tar xz -C "$_dest"
    chmod 755 "$_dest"/*.sh 2>/dev/null || true
    chmod 755 "$_dest/aka-capp" 2>/dev/null || true
    if [ -d "$_dest/tools" ]; then chmod 755 "$_dest"/tools/* 2>/dev/null || true; fi
    # macOS 打包可能带出来的元数据文件，清掉免干扰
    rm -f "$_dest"/._* 2>/dev/null || true
}

# staging 换包：解到 AKA_HOME.new → 保留用户文件 → 目录改名换入（旧目录留 .old）
swap_in() {
    _new="$AKA_HOME.new"
    _old="$AKA_HOME.old"
    rm -rf "$_new" "$_old"
    extract_payload "$_new"
    if [ "${AKA_OTA_RESET_CONFIG:-0}" != "1" ]; then
        for _f in $KEEP_FILES; do
            if [ -f "$AKA_HOME/$_f" ]; then
                cp -f "$AKA_HOME/$_f" "$_new/$_f"
                echo "[ota] 保留用户文件: $_f"
            fi
        done
    else
        echo "[ota] AKA_OTA_RESET_CONFIG=1 → config.toml 等一并覆盖"
    fi
    if [ -d "$AKA_HOME" ]; then mv "$AKA_HOME" "$_old"; fi
    mv "$_new" "$AKA_HOME"
    echo "[ota] 已换入新版本；回滚点: $_old"
}

stop_service() {
    # 先杀 capp，再杀守护脚本 init.sh（它是 while 循环，不停会 2 秒后把旧 capp 拉起）
    killall aka-capp 2>/dev/null || true
    pkill -f 'AKA-00/init.sh' 2>/dev/null || true
    pkill -f "$AKA_HOME/init.sh" 2>/dev/null || true
    sleep 1
    killall -9 aka-capp 2>/dev/null || true
}

case "$1" in
    --extract)
        echo "[ota] 解包到 ${AKA_HOME}（不重启）"
        extract_payload "$AKA_HOME"
        echo "[ota] 完成"
        exit 0
        ;;
    --init)
        echo "=== AKA-00 首次部署 ==="
        swap_in
        if [ -x "$AKA_HOME/init_ap_web.sh" ]; then "$AKA_HOME/init_ap_web.sh" || echo "[ota] init_ap_web.sh 失败（可稍后重试）"; fi
        echo "[ota] 部署完成，下次开机自动启动"
        exit 0
        ;;
    --update)
        echo "=== AKA-00 OTA 升级 ==="
        touch /tmp/aka-ota-lock            # 让守护脚本暂停拉起（若还在跑）
        stop_service
        swap_in
        rm -f /tmp/aka-ota-lock /tmp/aka-ota-install.sh /tmp/aka-ota-update
        echo "[ota] 重启服务..."
        exec "$AKA_HOME/init.sh"
        ;;
    *)
        if [ ! -x "$AKA_HOME/aka-capp" ]; then
            echo "[ota] 首次运行 → 解包到 $AKA_HOME"
            swap_in
        fi
        exec "$AKA_HOME/init.sh"
        ;;
esac
#__PAYLOAD_BELOW__
HEADER

# ── 回填 payload 偏移（定宽 7 位，长度不变，偏移才准）──
SIZE_BEFORE="$(wc -c < "$TMP_OUT" | tr -d ' ')"
OFFSET="$(printf '%07d' $((SIZE_BEFORE + 1)))"
TMPF="$(mktemp "${TMPDIR:-/tmp}/aka-ota.XXXXXX")"
sed "s/PAYLOAD_OFFSET=0000000/PAYLOAD_OFFSET=$OFFSET/" "$TMP_OUT" > "$TMPF"
mv "$TMPF" "$TMP_OUT"

# ── payload：先打成临时包并校验，再追加到头部 ──
# 为什么要容错 tar 的"file changed as we read it"：
#   源目录里常有别的进程在动文件（Finder 写 .DS_Store、编辑器索引、并发构建……），
#   tar 会把它当警告但**以 1 退出**，配合 set -e 会让整次发布随机失败，还会留下半截产物。
#   所以：排除易变文件 + --warning=no-file-changed，并且只接受退出码 0/1（1=仅警告）。
tar_rc=0
(
    cd "$SRC"
    COPYFILE_DISABLE=1 tar czf "$TMP_PAYLOAD" \
        --exclude='.DS_Store' --exclude='._*' --exclude='.fseventsd' \
        --exclude='.Spotlight-V100' --exclude='.ota' \
        --warning=no-file-changed . 2>/dev/null
) || tar_rc=$?
if [ "$tar_rc" -gt 1 ]; then
    echo "错误: 打包 payload 失败（tar 退出码 $tar_rc）" >&2
    exit 1
fi
[ "$tar_rc" = 1 ] && echo "  （注意：源目录在打包过程中被改动过，已按警告忽略）"

# payload 自检：能列出、且含 aka-capp，避免把空包/半包发出去
if ! tar tzf "$TMP_PAYLOAD" >/dev/null 2>&1; then
    echo "错误: payload 不是有效的 tar.gz" >&2
    exit 1
fi
if ! tar tzf "$TMP_PAYLOAD" | grep -q 'aka-capp'; then
    echo "错误: payload 里没有 aka-capp（$SRC 没打包对吗）" >&2
    exit 1
fi

cat "$TMP_PAYLOAD" >> "$TMP_OUT"
chmod 755 "$TMP_OUT"
mv -f "$TMP_OUT" "$OUT"      # 原子替换：此刻起 dist/aka-00-server 才是有效产物

SIZE="$(wc -c < "$OUT" | tr -d ' ')"
if command -v md5 >/dev/null 2>&1; then MD5="$(md5 -q "$OUT")"; else MD5="$(md5sum "$OUT" | cut -d' ' -f1)"; fi

echo "  ✓ 安装器: $OUT"
echo "    版本   : $VER"
echo "    大小   : $SIZE 字节 ($((SIZE / 1024)) KB)"
echo "    md5    : $MD5"
echo "    payload 偏移: $OFFSET"
echo
echo "  用法: ./aka-00-server [--init|--update|--extract]"
echo "  上传到云端更新源（imageUrl）即可被 /api/ota/upgrade 拉取安装"
