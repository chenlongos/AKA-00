# 快速开始

本文档帮助你 5 分钟内让 AKA-00 跑起来。

## 1. 组装

参考 [硬件接线](./03-hardware/wiring.md) 完成机械臂、电机、摄像头的连接。

## 2. 通电

1. 连接电源，等待控制板指示灯亮起
2. 等待 20-30 秒，网络模块启动
3. 连接机器人热点（格式：`chenlong-robot-xxxxx`）
4. 浏览器访问 `192.168.4.1`，配置 WiFi 连接

详细步骤见 [机器人连接](./04-setup/connection.md)

## 3. 运行

```bash
# SSH 登录控制板
ssh root@<机器人IP>

# 安装依赖
pip install -r requirements.txt
cd frontend && npm i && npm run build && cd ..

# 启动服务
python run.py
```

详细部署见 [软件部署](./04-setup/software.md)

## 4. 使用

启动后通过以下方式控制：

- **Web 界面**: 访问 `http://<机器人IP>/`
- **API**: 使用 `/api/control` 接口

详细说明见 [Web 界面](./05-usage/web-ui.md) 和 [API 文档](./05-usage/api.md)

## 下一步

- [配置 WiFi](./04-setup/wifi-config.md)
- [了解代码结构](./06-development/structure.md)
- [常见问题](./07-faq.md)
