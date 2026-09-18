# 快速开始

本文档帮助你快速开始让 AKA-00 跑起来。

## 1. 组装

参考 [硬件接线](./03-hardware/wiring.md) 完成机械臂、电机、摄像头的连接。

## 2. 通电

1. 连接电源，等待控制板指示灯亮起
2. 等待 60 秒，网络模块启动
3. 连接机器人热点（格式：`chenlong-robot-xxxxx`）
4. 浏览器访问 `192.168.4.1`，进入控制界面
5. 之后可以通过手机上的遥控器控制小车

## 3. 部署（首次/更新）

项目以单文件 `aka-00-server` 分发，拷贝到控制板：

```bash
# 打包（在开发机上）
make -C cpp ota                 # 打包（用已有前端产物）
cd frontend && npm run build    # 需要重建前端时先跑这个
make -C cpp ota                 # 再打包

# 拷贝到控制板
scp cpp/dist/aka-00-server root@<robot>:

# 首次部署：一键初始化（解压 + 热点 + 自启）
ssh root@<robot> 'aka-00-server --init'

# 之后每次开机自动启动，也可手动运行
ssh root@<robot> 'aka-00-server'
```

更新部署（**保留** `config.toml`、证书、demo 卡片等现场数据）：
```bash
scp cpp/dist/aka-00-server root@<robot>:/tmp/
ssh root@<robot> 'chmod +x /tmp/aka-00-server && /tmp/aka-00-server --update'
```
> 想让本次带的默认配置连 `config.toml` 一起覆盖，用 `AKA_OTA_RESET_CONFIG=1 ...--update`。

## 4. 修改代码常用命令

```bash
# SSH 登录控制板
ssh root@<机器人IP>

# 本地修改代码后，重新打包并部署
make -C cpp ota && scp cpp/dist/aka-00-server root@<robot>:/usr/local/bin/

# 在控制板上重启服务
ssh root@<robot> 'aka-00-server --update'
```

## 5. 使用

启动后通过以下方式控制：

- **Web 界面**: 访问 `http://<机器人IP>/`
- **API**: 使用 `/api/control` 接口

详细说明见 [Web 界面](./05-usage/web-ui.md) 和 [API 文档](./05-usage/api.md)

## 下一步

- [配置 WiFi](./04-setup/wifi-config.md)
- [了解代码结构](./06-development/structure.md)
- [常见问题](./07-faq.md)
