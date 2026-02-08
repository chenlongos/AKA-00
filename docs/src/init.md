# 第三章 基础配置

以下基于wifi版

## 3.1 连接网络
- 方式一、通过wifi指定直接连接网络

- 方式二、在win上，通过usb实现网络共享

```shell
ip addr flush dev usb0
ip addr add 192.168.137.100/24 dev usb0
ip link set usb0 up

ip route del default
ip route add default via 192.168.137.1

echo "nameserver 8.8.8.8" > /etc/resolv.conf
```

## 3.2 运行程序

```python
python3 car_control_server.py
```

后台启动
```python
nohup python3 car_control_server.py > app.log 2>&1 &
```

## 3.3 mDNS配置(暂没实现)

sg2002 内置支持了avahi，用
```shell
ps | grep avahi
```
可以发现
```shell
470 avahi avahi-daemon: running [licheervnano-366e.local]
```
为
<主机名>-<冲突后缀>.local

修改 /etc/hosts和 /etc/hostname

修改/etc/avahi/下的
avahi-daemon.conf

之后kill avahi的pid
之后 avahi-daemon -D

## 3.4 https服务需要生成证书
```shell
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 3650 -nodes
```

-  无交互生成自签名证书，有效期10年（3650天）
```shell
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 3650 -nodes -subj "/C=CN/ST=Beijing/L=Beijing/O=MyOrg/OU=MyDept/CN=localhost"
```

## 3.5 开机自启动

在/etc/init.d 文件中 添加 一个appinit文件，输入

```shell
#!/bin/sh
# 程序路径
APP_PATH="/root/AKA-00"
# 程序运行用户（一般嵌入式用 root）
RUN_USER="root"

# 启动函数
start() {
	chmod +x /root/AKA-00/init.sh
	/root/AKA-00/init.sh
}

# 停止函数（可选，便于手动管理）
stop() {}

# 重启函数（可选）
restart() {
    stop
    sleep 1
    start
}

# 脚本参数处理
case "$1" in
    start)
        start
        ;;
    stop)
        stop
        ;;
    restart)
        restart
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac

exit 0
```

之后在 /etc/inittab 中加入一行，就可以开机自启动，代码要放在AKA-00下

```
app::sysinit:/etc/init.d/appinit start
```

## 3.6 网络配置

```shell
ctrl_interface=/var/run/wpa_supplicant
ap_scan=1

network={
  ssid="wifi名"
  psk="wifi密码"
  priority=8
}

network={
  ssid="BoBoPhone"
  psk="********"
  priority=5
}

network={
  key_mgmt=NONE
  priority=1
}
```