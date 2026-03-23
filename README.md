# AKA-00

辰龙AI教育机器人

## 项目目标

AKA-00 是一个面向教学的低成本AI机器人，通过提供简单的平台实现多种算法的训练和仿真。

## 目前功能

### ✅ 已完成

| 功能 | 说明 |
|------|------|
| 机械臂控制 | 控制机械臂完成抓取和释放动作 |
| 底盘运动 | N20 电机差速控制，支持前进/后退/转向 |
| Web API | 提供 HTTP 接口远程控制机器人 |
| Web 界面 | React 前端，可远程控制机器人运动和夹爪 |

## 代码结构

```
AKA-00/
├── run.py              # Web 服务器入口
├── tennis_hunter.py    # 机器人主程序
├── app/                # Flask Web 应用
├── src/                # 硬件控制模块（机械臂、电机、摄像头）
├── frontend/           # React 前端
└── models/            # YOLOv8 模型文件

详细说明见 [文档](./docs/src/06-development/structure.md)
```

## 技术栈

### 后端

| 技术 | 用途 |
|------|------|
| Python 3.11 | 核心语言 |
| Flask | Web 框架 |
| pyserial | 串口通信（舵机） |
| python-periphery | PWM 控制 |

### 前端

| 技术 | 用途 |
|------|------|
| React 19 | UI 框架 |
| TypeScript | 类型安全 |
| Vite | 构建工具 |

### 硬件

| 设备    | 型号                |
|-------|-------------------|
| 开发板   | [LicheeRV Nano](https://wiki.sipeed.com/hardware/zh/lichee/RV_Nano/1_intro.html)<br/>        |
| 机械臂舵机 | ZL-ZP10S |
| 电机    | N20 直流减速电机        |

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
- **左电机**: chip=4, ch1=0, ch2=1
- **右电机**: chip=4, ch2=2, ch3=3