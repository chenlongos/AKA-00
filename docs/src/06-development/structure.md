# 代码结构

```
AKA-00/
├── run.py                      # 主入口，启动 HTTP/HTTPS 服务器
├── tennis_hunter.py            # 机器人主程序（网球收集逻辑）
├── requirements.txt            # Python 依赖
├── init.sh                     # 系统初始化脚本
│
├── app/                        # Flask Web 应用
│   ├── __init__.py             # Flask 应用工厂
│   └── routes/
│       ├── api.py              # 控制 API（运动、夹爪）
│       └── frontend.py         # 前端路由
│
├── src/                        # 硬件控制模块
│   ├── arm_control/            # 机械臂控制
│   │   ├── sts3215/            # STS3215 舵机驱动
│   │   ├── mg996r/             # MG996R 舵机
│   │   └── zl/zp10s/           # ZL-ZP10S 机械臂
│   ├── base_control/
│   │   └── n20/                # N20 电机驱动
│   └── cameras/
│       └── opencv/             # 摄像头模块
│
├── frontend/                   # React 前端
│   ├── src/
│   │   ├── App.tsx             # 主应用组件
│   │   ├── pages/
│   │   │   ├── BaseControlPage.tsx   # 遥控器页面
│   │   │   └── WiFiConfigPage.tsx     # WiFi配置页面
│   │   └── ...
│   ├── package.json
│   └── vite.config.ts
│
├── models/                      # YOLOv8 模型文件
│   ├── best.onnx               # CPU 推理模型
│   └── best.rknn               # RK3588 推理模型
│
├── static/                     # Flask 静态文件
└── templates/                  # Flask 模板
```

## 关键模块

| 文件 | 功能                        |
|------|---------------------------|
| `run.py` | Flask 服务器启动，支持 HTTP/HTTPS |
| `app/__init__.py` | Flask 应用工厂，初始化硬件驱动        |
| `app/routes/api.py` | HTTP API，提供运动/夹爪控制        |
| `src/arm_control/` | 舵机通信协议实现                  |
| `src/base_control/n20/` | PWM N20电机速度控制             |
