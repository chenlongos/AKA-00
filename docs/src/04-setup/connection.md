# 初始化配置

这部分为机器人的初始化部分，都会在用户拿到设备前实现，如果用户需要自行初始化也可以按照本流程实现。

## 烧录镜像

从[Releases](https://github.com/chenlongos/AKA-00/releases/)下载最新镜像，通过烧入工具将镜像烧录到tf卡中，镜像中会自带一份项目文件。

## 连接主控

通过type-c接口可以将板子连接到电脑上

在win下在终端里输入`ipconfig`，找到一个新的以太网，例如 `10.163.124.100`。
之后可以使用ssh进行连接，`ssh root@10.163.124.1`

## 部署 aka-server

将 `aka-server` 拷贝到控制板并配置热点和自启：

```shell
# 1. 拷贝单文件可执行程序到控制板
scp dist/aka-server root@<robot>:/usr/local/bin/

# 2. SSH 到控制板，运行初始化脚本（首次部署需要）
ssh root@<robot>
chmod +x init_ap_web.sh
./init_ap_web.sh

# 3. 启动服务
aka-server
```

`init_ap_web.sh` 做了以下事情：
- 创建 AP 热点（SSID: `chenlong-robot-xxxxx`，基于 MAC 地址唯一）
- 配置 DHCP（192.168.4.100-200）
- 添加 S99webstart 自启脚本，开机自动启动 `aka-server`

之后每次开机都会自动运行。如需手动更新：

```shell
scp dist/aka-server root@<robot>:/usr/local/bin/
ssh root@<robot> 'rm -rf /root/AKA-00 && aka-server'
```

## HTTPS 证书生成命令

-  无交互生成自签名证书，有效期10年（3650天）
```shell
openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -days 3650 -nodes -subj "/C=CN/ST=Beijing/L=Beijing/O=MyOrg/OU=MyDept/CN=localhost"
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