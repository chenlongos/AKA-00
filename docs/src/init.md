1. 连接网络
在win上，通过usb共享网络之后进行一些配置
```shell
ip addr flush dev usb0
ip addr add 192.168.137.100/24 dev usb0
ip link set usb0 up

ip route del default
ip route add default via 192.168.137.1

echo "nameserver 8.8.8.8" > /etc/resolv.conf
```

2. 配置python虚拟环境
```python
python -m venv chenlong
source chenlong/bin/activate
```

3. 下载代码


4. 安装依赖

```python
python -m pip install -r requirements.txt
```

5. 运行程序

```python
python car_control_server.py
```
后台启动##
```python
nohup python car_control_server.py > app.log 2>&1 &
```

6. mDNS配置
sg2002 内置支持了avahi，用
```shell
ps | grep avahi
```
可以发现
```shell
470 avahi    avahi-daemon: running [licheervnano-366e.local]
```
为
<主机名>-<冲突后缀>.local
