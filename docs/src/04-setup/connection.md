# 初始化配置

这部分为机器人的初始化部分，都会在用户拿到设备前实现，如果用户需要自行初始化也可以按照本流程实现。

## 烧录镜像

从[Releases](https://github.com/chenlongos/AKA-00/releases/)下载最新镜像，通过烧入工具将镜像烧录到tf卡中，镜像中会自带一份项目文件。

## 连接主控

通过type-c接口可以将板子连接到电脑上

在win下在终端里输入`ipconfig`，找到一个新的以太网，例如 `10.163.124.100`。
之后可以使用ssh进行连接，`ssh root@10.163.124.1`

## 部署 aka-00-server

将 `aka-00-server` 拷贝到控制板，一条命令完成初始化：

```shell
# 1. 拷贝到控制板
scp cpp/dist/aka-00-server root@<robot>:/usr/local/bin/

# 2. 一键初始化（解压 + AP 热点 + DHCP + 开机自启）
ssh root@<robot> 'aka-00-server --init'
```

`--init` 自动完成：
- 解压项目文件到 `$HOME/AKA-00`
- 配置 AP 热点（SSID: `chenlong-robot-xxxxx`，基于 MAC 地址唯一）
- 配置 DHCP（192.168.4.100-200）
- 写入 S98apstart / S99webstart 自启脚本
- 立即启动热点

之后每次开机自动运行 `aka-00-server`。如需手动更新：

```shell
scp cpp/dist/aka-00-server root@<robot>:/usr/local/bin/
ssh root@<robot> 'aka-00-server --update'
```

## HTTPS 证书生成命令

> 通常**不需要手工执行** —— `init.sh` 启动前会调 `https_init.sh` 自动生成
> （缺 `cert.pem`/`key.pem` 时），端口默认 443。下面这条只在你想自己换/重置证书时用。

-  无交互生成自签名证书，有效期10年（3650天）
```shell
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout key.pem -out cert.pem -days 3650 -nodes \
    -subj "/C=CN/ST=Beijing/L=Beijing/O=MyOrg/OU=MyDept/CN=localhost"
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