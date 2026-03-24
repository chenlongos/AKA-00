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

## 代码修改完成后部署到控制板

修改完代码后先在本地查看修改和调用，确认无误后可以通过scp命令进行传输，要将修改了的文件替换主控上对应的文件。
```
scp 本地文件 root@<ip>：
```

之后通过 ps 命令确认当前是否有服务启动占用80和443端口，如果有会返回pid
```
ps | grep python*
```

并用 kill 关闭目标进程
```
kill -9 <pid>
```


之后再通过
```
python run.py
```

在主控板上启动项目进行调试