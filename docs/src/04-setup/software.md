# 项目本地启动并部署到控制板

## 项目本地启动

1. 安装Miniconda用于控制python的版本

安装miniconda，请按照官方[安装指南](https://docs.anaconda.net.cn/miniconda/install/)

创建 python 3.11 环境
```shell
conda create -n aka python=3.11 -y
```

2. 运行 pip install -r requirements.txt 安装依赖
```shell
pip install -r requirements.txt
```

3. 安装前端依赖
```shell
cd frontend && npm i
```

4.打包前端项目

```shell
npm run build && cd ..
```

5. 运行项目

```shell
python run.py
```
之后访问本地的80端口或443端口即可

本地对于硬件调用的接口进行了隔离，所以可以直接启动

## 打包部署到控制板

使用 `build_release.sh` 将整个项目打包为单个自解压可执行文件 `aka-server`，然后拷贝到 SG2002 控制板即可运行。

```bash
# 构建（在开发机上执行）
./build_release.sh              # 使用已有静态文件
./build_release.sh --rebuild    # 自动构建前端后打包

# 输出: dist/aka-server (约 9MB)
```

### 首次部署

```bash
# 1. 拷贝 aka-server 到控制板
scp dist/aka-server root@<robot>:/usr/local/bin/

# 2. SSH 到控制板，运行初始化脚本（配置热点 + 开机自启）
ssh root@<robot>
chmod +x init_ap_web.sh && ./init_ap_web.sh

# 3. 启动服务
aka-server
```

### 更新部署

重新构建后覆盖即可，需要清除旧数据目录：

```bash
scp dist/aka-server root@<robot>:/usr/local/bin/
ssh root@<robot> 'rm -rf /root/AKA-00 && aka-server'
```

### 工作原理

`aka-server` 是一个自解压程序：
1. 首次运行时自动解压项目文件到 `${AKA_HOME:-/root/AKA-00}`
2. 执行 `uart_init.sh` 初始化串口（如果存在）
3. 启动 `python3 run.py` 运行 Web 服务

后续运行时跳过解压步骤，直接启动服务。
