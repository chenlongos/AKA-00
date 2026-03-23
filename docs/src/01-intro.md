# 项目介绍

AKA-00 是一个面向教学的低成本AI机器人，通过提供简单的平台实现多种算法的训练和仿真。

## 核心能力

| 能力 | 说明 |
|------|------|
| 机械臂控制 | 支持 STS3215、MG996R 等舵机 |
| 底盘运动 | N20 电机差速控制 |
| 远程控制 | Web 界面 + HTTP API |

## 技术架构

```
AKA-00
├── tennis_hunter.py     # 机器人主程序
├── run.py               # Web 服务器
├── src/
│   ├── arm_control/     # 机械臂控制（舵机驱动）
│   ├── base_control/    # 底盘控制（电机驱动）
│   └── cameras/         # 摄像头模块
├── app/                 # Flask Web 应用
├── frontend/            # React 前端
└── models/              # YOLOv8 模型
```

## 硬件平台

| 组件 | 型号 |
|------|------|
| 主控 |  [LicheeRV Nano](https://wiki.sipeed.com/hardware/zh/lichee/RV_Nano/1_intro.html) |
| 机械臂 | ZL-ZP10S / STS3215 |
| 电机 | N20 直流减速电机 |
| 摄像头 | USB 免驱摄像头 |
