# AKA-00

开源智能网球收集机器人

## 项目目标

AKA-00 是一个面向网球场地的智能机器人，旨在实现**自动收集散落网球**的功能。通过计算机视觉识别网球位置，配合机械臂抓取和底盘运动，实现无人值守的网球收集作业。

## 目前功能

### ✅ 已完成

| 功能 | 说明 |
|------|------|
| 网球检测 | 使用 YOLOv8 模型识别网球（支持 ONNX/RKNN 格式） |
| 机械臂控制 | 控制机械臂完成抓取和释放动作 |
| 底盘运动 | N20 电机差速控制，支持前进/后退/转向 |
| Web API | 提供 HTTP 接口远程控制机器人 |
| Web 界面 | React 前端，可远程控制机器人运动和夹爪 |

## 使用流程

### 1. 硬件连接




// TODO

### 2. 软件部署

```bash
# 安装 Python 依赖
pip install -r requirements.txt

# 初始化系统（可选）
./init.sh

# 启动 Web 服务器
python run.py
```

### 3. 远程控制

启动后可通过以下方式控制：

1. **Web 界面**: 访问 `http://<机器人IP>/`
2. **API 调用**: 使用 `/api/control` 接口

### 4. 启动自动收集

```bash
python tennis_hunter.py
```

机器人将执行：检测网球 → 追踪接近 → 机械臂抓取 → 寻找红色桶 → 释放

## 代码结构

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
│   ├── arm_control/           # 机械臂控制
│   │   ├── sts3215/           # STS3215 舵机驱动
│   │   ├── mg996r/           # MG996R 舵机
│   │   └── zl/zp10s/         # ZL-ZP10S 机械臂
│   ├── base_control/
│   │   └── n20/               # N20 电机驱动
│   └── cameras/
│       └── opencv/            # 摄像头模块
│
├── frontend/                   # React 前端
│   ├── src/
│   │   ├── App.tsx            # 主应用组件
│   │   ├── pages/BaseControlPage.tsx    # 遥控器页面
│   │   ├── pages/WiFiConfigPage.tsx     # Wifi配置页面
│   │   └── ...
│   ├── package.json
│   └── vite.config.ts
│
├── models/                    # YOLOv8 模型文件
│   ├── best.onnx             # CPU 推理模型
│   └── best.rknn             # RK3588 推理模型
│
├── static/                    # Flask 静态文件
└── templates/                 # Flask 模板
```

### 关键模块说明

| 文件 | 功能 |
|------|------|
| `run.py` | Flask 服务器启动，支持 HTTP/HTTPS |
| `tennis_hunter.py` | 网球收集主逻辑，包含目标检测和控制流程 |
| `app/__init__.py` | Flask 应用工厂，初始化硬件驱动 |
| `app/routes/api.py` | HTTP API，提供运动/夹爪控制 |
| `src/arm_control/sts3215/` | 串口舵机通信协议实现 |
| `src/base_control/n20/` | PWM 电机速度控制 |

## 技术栈

### 后端

| 技术 | 用途 |
|------|------|
| Python 3.11 | 核心语言 |
| Flask | Web 框架 |
| pyserial | 串口通信（舵机） |
| python-periphery | GPIO/PWM 控制 |
| OpenCV | 图像处理 |
| Ultralytics | YOLOv8 推理 |

### 前端

| 技术 | 用途 |
|------|------|
| React 19 | UI 框架 |
| TypeScript | 类型安全 |
| Vite | 构建工具 |

### 硬件

| 设备 | 型号 |
|------|------|
| 开发板 | 瑞芯微 RK3588 |
| 机械臂 | ZL-ZP10S / STS3215 |
| 电机 | N20 直流减速电机 |
| 摄像头 | USB 免驱摄像头 |

## 环境变量

| 变量名 | 默认值 | 说明 |
|--------|--------|------|
| `APP_HTTP_PORT` | 80 (Linux) / 5000 (Windows) | HTTP 服务端口 |
| `APP_HTTPS_PORT` | 443 (Linux) / 5443 (Windows) | HTTPS 服务端口 |
| `APP_CERT_PATH` | /root/AKA-00/cert.pem | HTTPS 证书路径 |
| `APP_KEY_PATH` | /root/AKA-00/key.pem | HTTPS 密钥路径 |

## API 接口

### 获取 IP 地址

```bash
GET /api/ip
```

响应：
```json
{
  "ip": "192.168.1.100"
}
```

### 控制接口

```bash
GET /api/control?action=<action>&speed=<speed>&time=<time>
```

参数说明：

| 参数 | 类型 | 说明 |
|------|------|------|
| action | string | 动作：`up`, `down`, `left`, `right`, `stop`, `grab`, `release` |
| speed | int | 速度 0-50 |
| time | int | 持续时间（毫秒） |

示例：

```bash
# 前进
curl "http://192.168.1.100/api/control?action=up&speed=30&time=1000"

# 抓取
curl "http://192.168.1.100/api/control?action=grab"

# 释放
curl "http://192.168.1.100/api/control?action=release"
```

## 硬件配置详情

### 机械臂

- **型号**: ZL-ZP10S 或 STS3215
- **通信**: 串口 UART (`/dev/ttyACM0`, 115200 波特率)

### 电机

- **型号**: N20 直流减速电机
- **控制**: PWM 调速
- **左电机**: chip=4, ch1=0, ch2=1 (RK3588)
- **右电机**: chip=4, ch2=2, ch3=3 (RK3588)

### 摄像头

- **接口**: USB 免驱
- **默认设备**: `/dev/video0`

## 运行模式

在 `tennis_hunter.py` 中配置：

```python
HARDWARE_MODE = 'rk3588'  # 或 'cpu'
```

| 模式 | 模型 | 说明 |
|------|------|------|
| `cpu` | best.onnx | 通用 PC 推理 |
| `rk3588` | best.rknn | RK3588 NPU 推理 |
