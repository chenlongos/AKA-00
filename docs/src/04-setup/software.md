# 项目本地启动并部署到控制板

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

修改完代码后先在本地查看修改和调用，确认无误后可以通过
```scp 本地文件 root@<ip>：```
的方式将修改了的文件替换到主控上，

之后通过
```ps | grep python*```
找到当前端口占用的进程

并用
```kill -9 <pid>```
关闭

之后再通过

```python run.py```

启动项目进行调试