#!/bin/bash
set -e

echo "=== AKA-00 Systemd 服务部署 (RK3576) ==="

SERVICE_FILE="$(cd "$(dirname "$0")" && pwd)/services/aka-00.service"
TARGET="/etc/systemd/system/aka-00.service"

if [ ! -f "$SERVICE_FILE" ]; then
    echo "❌ service 文件不存在: $SERVICE_FILE"
    exit 1
fi

# ── 0. 系统权限修复 ───────────────────────────────────────────────────────────
echo "修复系统权限..."

# 检查 sudo 是否可用（/etc/sudo.conf 属主错误会导致 sudo 完全不可用）
if ! sudo -n true 2>/dev/null; then
    echo "⚠️  sudo 不可用（可能 /etc/sudo.conf 属主错误）"
    echo "   尝试直接修复..."
    if [ -f /etc/sudo.conf ] && [ "$(stat -c '%u' /etc/sudo.conf 2>/dev/null)" != "0" ]; then
        # 尝试用当前 shell 权限直接修复
        chown root:root /etc/sudo.conf 2>/dev/null || {
            echo "❌ 无法修复 /etc/sudo.conf 属主（只读文件系统或权限不足）"
            echo "   请以 root 身份手动执行: chown root:root /etc/sudo.conf"
            echo "   或在烧录镜像时修复此问题"
            exit 1
        }
        echo "  ✅ /etc/sudo.conf 属主已修复"
    fi
    # 再次检查
    if ! sudo -n true 2>/dev/null; then
        echo "❌ sudo 仍然不可用，请手动修复后重试"
        exit 1
    fi
fi

# 修复 /etc/sudo.conf 属主（容器/烧录环境下可能错误）
if [ -f /etc/sudo.conf ]; then
    SUDO_OWNER=$(stat -c '%u' /etc/sudo.conf 2>/dev/null || echo "0")
    if [ "$SUDO_OWNER" != "0" ]; then
        echo "  修复 /etc/sudo.conf 属主 (当前 uid=$SUDO_OWNER)..."
        sudo chown root:root /etc/sudo.conf
        echo "  ✅ /etc/sudo.conf 已修复"
    else
        echo "  /etc/sudo.conf 属主正常"
    fi
fi

# 确保 netdev 组存在
if ! getent group netdev >/dev/null 2>&1; then
    echo "  创建 netdev 组..."
    sudo groupadd netdev
fi

# 将用户 cat 加入 netdev 组（用于访问 wpa_supplicant 控制接口）
if ! id -nG cat 2>/dev/null | grep -qw netdev; then
    echo "  将用户 cat 加入 netdev 组..."
    sudo usermod -aG netdev cat
    echo "  ✅ cat 已加入 netdev 组（需重新登录生效）"
else
    echo "  用户 cat 已在 netdev 组中"
fi

# 确保 wpa_supplicant 控制接口目录存在且权限正确
sudo mkdir -p /var/run/wpa_supplicant
sudo chown root:netdev /var/run/wpa_supplicant 2>/dev/null || true
sudo chmod 775 /var/run/wpa_supplicant
echo "  ✅ /var/run/wpa_supplicant 权限已设置"

echo ""

# ── 1. 部署 service 文件 ──────────────────────────────────────────────────────
sudo cp "$SERVICE_FILE" "$TARGET"
sudo chmod 644 "$TARGET"
echo "✅ 已安装: $TARGET"

# 重新加载 systemd
sudo systemctl daemon-reload
echo "✅ systemd daemon-reload 完成"

# 确保 init_ap_web.sh 可执行
chmod +x "$(cd "$(dirname "$0")" && pwd)/init_ap_web.sh" 2>/dev/null || true

# 停止旧服务（如果有）
sudo systemctl stop aka-00.service 2>/dev/null || true

# 启用开机自启
sudo systemctl enable aka-00.service
echo "✅ 已启用开机自启"

# 启动服务
sudo systemctl start aka-00.service
sleep 2

# 检查状态
if systemctl is-active --quiet aka-00.service; then
    echo "✅ 服务已启动"
    systemctl status aka-00.service --no-pager -l | head -10
else
    echo "⚠️  服务启动失败，查看日志:"
    journalctl -u aka-00.service --no-pager -n 20
fi

echo ""
echo "管理命令:"
echo "  查看状态: systemctl status aka-00"
echo "  查看日志: journalctl -u aka-00 -f"
echo "  重启:     systemctl restart aka-00"
echo "  停止:     systemctl stop aka-00"
echo "  启动:     systemctl start aka-00"
