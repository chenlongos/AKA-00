# 在pc上项目启动方法

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