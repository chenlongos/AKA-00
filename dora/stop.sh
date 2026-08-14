#!/bin/sh
# dora 本地停止（dev/init 用）。和 build_release.sh 里 generated 的 stop.sh 是两套独立实现：
# 那个走 riscv64 板子（守护进程、/dev/shm、$DORA_HOME 等）；这个走 macOS / 普通 Linux dev box。
# 共享思路（pgid kill + watch）但实现细节不同。
#
# 注意：故意不开 `set -e`——`pgrep` / `kill` 在进程已死时返回非 0，
# `set -e` 会让 SIGTERM 那一轮就 abort，永远到不了 SIGKILL。

echo "🛑 Stopping dora..."

# 1. dora CLI 自己的 stop/destroy（多数情况 no-op，但有 daemon 进程时可省清理）
dora stop 2>/dev/null || true
dora destroy 2>/dev/null || true

# 2. SIGTERM graceful（让 dora node 刷完 buffer / 关闭 serial）
#    名单和 build_release.sh 那份对齐 (dev 子集)。
for name in camera-node web-server motor-bridge arm-bridge demo-node state-node screen-node dora-daemon dora-coordinator; do
    pgrep -f "$name" 2>/dev/null | while read pid; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
done

# 3. 给节点 2s 清理（SIGTERM 后 dora-node 一般 200-500ms 就退出，留余量）
#    之前是 0.3s，太短——节点在 flush buffer 时被强杀有 fs 风险。
sleep 2

# 4. SIGKILL 兜底
for name in camera-node web-server motor-bridge arm-bridge demo-node state-node screen-node dora-daemon dora-coordinator; do
    pgrep -f "$name" 2>/dev/null | while read pid; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null || true
    done
done

# 5. 释放 web 端口——lsof 跨 Linux/macOS 都有，fuser 在 macOS BSD 上不支持 -k
#    （BSD fuser 只 print PIDs 不杀进程），所以统一走 lsof。HTTP/80 拿 PID 后 kill。
kill_port() {
    # lsof -ti tcp:PORT 输出持有该端口的 PID 列表；失败（端口无人占用）静默忽略
    for pid in $(lsof -ti tcp:"$1" 2>/dev/null); do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null || true
    done
}
kill_port 80      # web-server
kill_port 5000    # vite dev server (forwarded /api)

# 6. /dev/shm 清理（如果有，buildroot 才有）
rm -f /dev/shm/dora-* 2>/dev/null || true
rm -f /dev/shm/zenoh-* 2>/dev/null || true
# shmem_* = dora arrow IPC 一次性共享内存残留，异常退出/重启会堆积，把板子内存挤爆
rm -f /dev/shm/shmem_* 2>/dev/null || true

echo "✅ Dora stopped"
