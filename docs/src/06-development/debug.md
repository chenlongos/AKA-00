# 调试方法

## 串口连接

```bash
# 安装 minicom
sudo apt install minicom

# 连接串口
minicom -D /dev/ttyUSB0 -b 115200
```

## 网络连接

### USB RNDIS 网口

```bash
# 查看网口
ip a show

# 如果主机是 10.245.118.100，则开发板是 10.245.118.1
ssh root@10.245.118.1
```

### WiFi SSH

```bash
ssh root@<机器人IP>
```

## 日志查看

```bash
# 查看运行日志
cat app.log

# 实时查看日志
tail -f app.log
```

## 测试硬件

```bash
# 测试电机
python car_test.py
```

## 网络诊断

```bash
# 检测网络连通性
ping www.baidu.com

# 检测外网访问
curl www.baidu.com
```

## HTTPS 证书

如需启用 HTTPS，需生成自签名证书：

```bash
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 3650 -nodes -subj "/C=CN/ST=Beijing/L=Beijing/O=MyOrg/OU=MyDept/CN=localhost"
```
