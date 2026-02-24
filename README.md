# AKA-00
阿卡0号开源机器人

## 项目本地运行方法

1. 安装Miniconda用于控制python的版本



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