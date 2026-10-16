# 代码结构

板上跑的一切都在 `cpp/` 里：编译成单个 riscv64 静态二进制 `aka-capp`，加上配置和前端
产物，一起打进 `aka-00-server` 安装器（见[快速开始](../02-quickstart.md)）。根目录其余
部分是 Python 版（历史实现）的遗留文件，不再维护。

## 目录

```
AKA-00/
├── cpp/
│   ├── capp/                    # aka-capp 服务进程
│   │   ├── src/main.cpp         #   入口：读配置 → 起服务
│   │   ├── src/http/            #   自研 HTTP 服务（socket + 线程 + 路由 + 报文）
│   │   ├── src/routes/          #   接口路由，一个域一个文件（motor/camera/arm/demo/…）
│   │   ├── src/services/        #   业务服务：控制、摄像头、demo、推理、WiFi、上报
│   │   ├── src/websocket.cpp    #   /ws/control 二进制通道
│   │   ├── src/script.cpp       #   内置动作状态机（grab / approach）
│   │   └── include/capp/        #   头文件（context.hpp 是全局上下文）
│   │
│   ├── csrc/                    # 平台与硬件层（编成 libcsrc.a，不依赖 capp）
│   │   ├── src/camera/          #   V4L2 采集 + JPEG 解码
│   │   ├── src/display/         #   板载 SPI 屏显示（仅带屏版编译）
│   │   ├── src/motor_pair.cpp   #   差速运动学
│   │   ├── src/tt_pid.cpp       #   ESP32 底盘串口协议（闭环）
│   │   ├── src/gripper.cpp      #   夹爪
│   │   ├── src/zp10s.cpp        #   ZL-ZP10S 舵机
│   │   ├── src/sts3215.cpp      #   STS3215 舵机
│   │   ├── src/yolo*.cpp        #   NPU 推理（.cvimodel）
│   │   ├── src/{config,state,serial,system_utils,angle_config}.cpp
│   │   ├── src/{http_client,https_client}.cpp  # OTA / 状态上报的出栈请求
│   │   ├── tools/               #   板测工具：cam_probe / tt_pid_test / screen_test
│   │   └── include/csrc/        #   头文件
│   │
│   ├── board/                   # 板上 $AKA_HOME/ 的镜像（实体文件，见下）
│   ├── scripts/                 # 第三方库构建（libjpeg / mbedTLS）+ 安装器打包
│   └── third_party/             # 交叉编译产物（构建时生成，不进版本库）
│
├── frontend/                    # React 前端源码，构建产物写到 ../static
├── static/                      # 前端构建产物（打包时收进部署目录）
├── tests/                       # 板测 demo 源码（demo_camera.c 等）与测试脚本
├── hardware/                    # 原理图、舵机手册
└── docs/                        # 本文档（mdBook）
```

## 板上目录（`$AKA_HOME`）

部署到板子上的内容 = `cpp/board/` 原样 + 三样**构建产物**（`aka-capp`、`static/`、
`tools/`）。所以"这个文件是哪来的"只有两种答案：要么在 `cpp/board/` 里，要么是编出来的。

```
$AKA_HOME/
├── aka-capp            # riscv64 静态二进制（构建产物）
├── static/             # 前端页面（构建产物）
├── tools/              # 板测工具（构建产物）
├── config.toml         # 唯一配置文件：端口、串口、摄像头、屏、OTA 地址
├── init.sh / stop.sh   # 启动（自愈循环，崩溃自动重启）/ 停止
├── init_ap_web.sh      # 装 AP 热点 + 开机自启（一次性，无状态）
├── S97akaap            # 首次开机自举 AP（带幂等锁）
├── https_init.sh       # 缺证书时自动生成自签证书
├── demo/               # models/（动作已内置进 aka-capp）+ configs/（卡片）
├── arm_angles.json     # 机械臂标定角度（现场数据）
├── arm_angles_default.json
├── speed_config.json   # 行驶速度（现场数据）
├── VERSION             # 版本号（OTA 比对用）
└── start_img.jpg       # 屏显示待机图
```

## 想改什么，改哪里

| 想改什么 | 改哪里 |
|---|---|
| 端口、串口、摄像头、屏、OTA 地址 | `cpp/board/config.toml` |
| 抓取/接近这类流程与调参 | 动作内置在 `capp/script.cpp`（C++ 状态机）；参数改**卡片配置**（界面上建的，存在 `demo/configs/`） |
| 开机、热点、自启 | `cpp/board/init.sh`、`init_ap_web.sh`、`S97akaap` |
| 某个接口的行为 | `cpp/capp/src/routes/` 里对应域的文件 |
| 硬件时序、串口协议 | `cpp/csrc/src/` 里对应模块 |
| 前端页面 | `frontend/src/`，改完 `npm run build` 再打包 |

> **别在板子上直接改文件**：OTA 升级是整目录换包，只有 `config.toml`、`speed_config.json`、
> `arm_angles.json`、`cert.pem`、`key.pem` 这几个文件，以及 `demo/configs/`（板上优先）和
> `demo/models/`（取并集）会被保留，其余一律按包里的结算。想改什么就改仓库，
> 走 `make -C cpp ota` 重新打包部署。
