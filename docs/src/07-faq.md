# 常见问题

## 连接问题

### Q: 机器人热点无法连接？

1. 确保机器人已通电且指示灯亮起
2. 等待 60 秒让网络模块完全启动
3. 确认电脑/手机 WiFi 已开启

### Q: 无法 SSH 登录？

1. 确认电脑和机器人在同一 WiFi 网络
2. 检查 IP 地址是否正确
3. 尝试使用串口连接调试

### Q: WiFi 配置页面打不开？

浏览器访问 `192.168.4.1`，确认已连接机器人热点。

---

## 运行问题

### Q: 启动失败，提示缺少依赖？

```bash
pip install -r requirements.txt
```

### Q: 机械臂不响应？

1. 检查串口连接是否正确
2. 确认舵机供电正常
3. 检查 `/dev/ttyACM0` 设备是否存在

### Q: 电机不转动？

1. 检查 GPIO 连接
2. 确认 PWM 引脚配置正确
3. 检查电机供电

### Q: 摄像头无法识别？

1. 检查 USB 连接
2. 确认设备文件 `/dev/video0` 存在
3. 测试摄像头：`ls -l /dev/video0`

---

## 其他

### Q: 如何查看机器人 IP？

访问 `http://<机器人IP>/api/ip`

### Q: 如何开启 HTTPS？

1. 生成证书：
```bash
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 3650 -nodes
```

2. 配置环境变量：
```bash
export APP_CERT_PATH=/path/to/cert.pem
export APP_KEY_PATH=/path/to/key.pem
```

### Q: 如何设置开机自启？

参考 [机器人连接](./04-setup/connection.md) 中的开机自启配置。
