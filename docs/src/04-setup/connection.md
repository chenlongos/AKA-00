# 初始化配置

这部分为机器人的初始化部分，都会在用户拿到设备前实现，如果用户需要自行初始化也可以按照本流程实现。

## 烧录镜像
从Releases处下载最新镜像，通过烧入工具将镜像烧录到tf卡中，镜像中会自带一份项目文件。

## 连接主控

通过type-c接口可以将板子连接到电脑上

在win下在终端里输入`ipconfig`，找到一个新的以太网，例如 `10.163.124.100`。
之后可以使用ssh进行连接，`ssh root@10.163.124.1`

## 连接网络

启动一次根目录下的`init_ap_web.sh`这会让小车自己成为一个热点，方便个人设备的连接，同时将项目自启动脚本写入系统。

## 项目启动

可以选择重启机器人，或者手动启动项目。
```shell
chmod +x init.sh
./init.sh
```

如果没有生成过证书文件，会生成https的证书后运行项目

## HTTPS 证书生成
-  无交互生成自签名证书，有效期10年（3650天）
```shell
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 3650 -nodes -subj "/C=CN/ST=Beijing/L=Beijing/O=MyOrg/OU=MyDept/CN=localhost"
```

如果想通过指定 WiFi 的方式连接项目并自启动，可以遵循以下流程。

## 开机自启动

设置为sta模式，[修改方式](https://wiki.sipeed.com/hardware/zh/lichee/RV_Nano/5_peripheral.html#WIFI)

在/etc/init.d 文件中 添加 一个appinit文件，输入

```shell
#!/bin/sh
# 程序路径
APP_PATH="/root/AKA-00"
# 程序运行用户（一般嵌入式用 root）
RUN_USER="root"

# 启动函数
start() {
    sleep 5
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

## 网络配置
修改 /etc/wpa_supplicant.conf 文件
```shell
ctrl_interface=/var/run/wpa_supplicant
ap_scan=1

network={
  ssid="wifi名"
  psk="wifi密码"
  priority=8
}

network={
  ssid="#####"
  psk="********"
  priority=5
}

network={
  key_mgmt=NONE
  priority=1
}
```