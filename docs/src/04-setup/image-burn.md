# 镜像烧录

本文档介绍如何将系统镜像烧录到开发板。

## 准备工作

### 所需工具

- SD 卡（2GB或以上）
- SD 卡读卡器
- BalenaEtcher 或其他镜像烧录工具

### 下载镜像

从项目 releases 页面下载最新的系统镜像文件（`.img` 格式）。
[镜像文件地址](https://github.com/chenlongos/AKA-00/releases/)

## 烧录步骤

### Windows 系统

1. 插入 SD 卡
2. 打开 BalenaEtcher
3. 点击 "Flash from file"，选择下载的镜像文件
4. 点击 "Select target"，选择 SD 卡设备
5. 点击 "Flash" 开始烧录
6. 等待烧录完成（约 5-10 分钟）

### macOS / Linux 系统

使用 `dd` 命令烧录：

```shell
# 查看 SD 卡设备名称
diskutil list

# 卸载 SD 卡（假设设备为 /dev/disk2）
diskutil unmountDisk /dev/disk2

# 烧录镜像（注意：of 后面的设备名称不能包含 partition 号）
sudo dd if=./aka-image.img of=/dev/rdisk2 bs=4M status=progress

# 烧录完成后弹出 SD 卡
diskutil eject /dev/disk2
```

## 首次启动

1. 烧录完成后，将 SD 卡插入开发板的 SD 卡槽
2. 连接电源
3. 等待系统启动（约 1-2 分钟）
4. 通过串口或网络连接进行后续配置，详见[初始化配置](./connection.md)

### 常见问题

#### 无法启动

- 检查电源是否正常供电
- 确认 SD 卡已正确插入
- 尝试重新烧录镜像