# cpp/ — AKA-00 机器人 C++ 实现（SG2002 独立运行）

纯 C++（自研 JSON / HTTP / WebSocket / 串口，零第三方运行时依赖）重写机器人控制与
Web 服务：**csrc**（硬件控制库，对应原 Python `src/`）+ **capp**（Web 服务，对应原
Python `app/`）。产出 riscv64 musl 静态二进制，板子上一条命令启动，不依赖 Python 或
任何外部运行时。

```
cpp/
├── csrc/                     ← src/ 的 C++ 移植（硬件控制静态库 libcsrc.a）
│   ├── include/csrc/
│   │   ├── json.hpp          JSON 解析/序列化（自研）
│   │   ├── log.hpp           日志（stderr，CSRC_LOG_LEVEL 控制）
│   │   ├── serial.hpp        串口封装（termios 8N1 + poll 超时）
│   │   ├── base64.hpp / sha1.hpp    base64 + SHA-1（WebSocket 握手用）
│   │   ├── config.hpp        配置（config.toml 子集解析）
│   │   ├── angle_config.hpp  arm_angles.json 读写/迁移（对应 angle_config.py）
│   │   ├── tt_pid.hpp        TT 马达 ESP32 底盘 UART 协议（对应 tt_pid/__init__.py）
│   │   ├── motor_pair.hpp    MotorPair 接口 + 工厂 + Mock（对应 base_control/interfaces.py）
│   │   ├── zp10s.hpp         ZP10S 舵机驱动（对应 zl/zp10s/uart_control.py）
│   │   ├── sts3215.hpp       STS3215 总线舵机驱动（对应 sts3215/__init__.py）
│   │   ├── gripper.hpp       Gripper 接口 + 适配器 + 工厂（对应 arm_control/interfaces.py）
│   │   ├── camera.hpp        V4L2 + libjpeg 摄像头（参考 tests/demo_camera.c）
│   │   ├── screen_display.hpp 摄像头画面 → 板载 SPI 屏（/dev/fb0，ST7796S 320x480）
│   │   ├── state.hpp         RobotStatus + StateCollector（对应 src/state/__init__.py）
│   │   ├── system_utils.hpp  IP / MAC / CPU / 内存 / 磁盘 / uptime
│   │   └── http_client.hpp   极简 HTTP 客户端（https 走 curl 兜底）
│   └── src/*.cpp
├── capp/                     ← app/ 的 C++ 移植（独立 HTTP+WS 服务）
│   ├── include/capp/
│   │   ├── context.hpp       应用共享状态（服务单例 + demo/ota 状态）
│   │   ├── http_server.hpp   极简 HTTP 服务器（路由/CORS/静态文件/流式响应）
│   │   ├── websocket.hpp     RFC 6455 WebSocket（/ws/control 二进制协议）
│   │   └── routes.hpp
│   ├── src/
│   │   ├── main.cpp          入口（初始化硬件 → 启动 HTTP）
│   │   ├── services.cpp      控制服务 / 摄像头服务 / 云端状态上报
│   │   ├── routes.cpp        全部路由（control/motor/arm/camera/demo/ota/system/wifi/config）
│   │   └── websocket.cpp / http_server.cpp
│   └── scripts/              build-libjpeg.sh（交叉 libjpeg）/ init.sh / stop.sh
└── README.md
```

## 前端页面如何组合

前端不参与构建流程 —— 直接使用仓库根 `static/`（已构建好的产物）：

```text
static/ (index.html + assets/)  --打包-->  板上 $AKA_HOME/static/
```

- capp 启动时服务 `$AKA_HOME/static/`：`GET /` → `static/index.html`，
  `/assets/*` → 静态资源，非 API 路径 SPA fallback 回 `index.html`
- 前端源码在仓库 `frontend/`（React + Vite，`src/api.ts` 调 `/api/*`、
  `src/ControlSocket` 连 `/ws/control`）；以后若需改页面，改完在
  `frontend/` 里 `npm run build` 刷新根 `static/` 再重新打包即可
- 页面与 capp 的接口契约（REST + WS 二进制协议）见下节

## 构建

### 用 make 构建（orb / macOS 通用）

顶层 `cpp/Makefile` 用 make 驱动整个构建。**在 orb 里直接 `make`**；在 macOS 上
`make` 会自动经 `orb run` 转发交叉编译（产物落在共享目录）：

```sh
cd cpp

# ── 带屏版本（默认）──
make                      # 全流程: libjpeg → mbedtls → csrc → capp(riscv64) → package
make screen               # 同 make（显式）
# → 产物: capp/bin/aka-capp、dist/AKA-00/、dist/aka-capp.tar.gz

# ── 不带屏版本（整个显示栈编译期裁掉）──
make noscreen
# → 产物: capp/bin/aka-capp-noscreen、dist-noscreen/AKA-00/、dist-noscreen/aka-capp-noscreen.tar.gz

make libjpeg              # 交叉编译 libjpeg（首次自动下载源码）
make mbedtls              # 交叉编译 mbedTLS（首次自动下载源码，HTTPS 用）
make csrc | capp | tools | package                      # 带屏版各步骤
make csrc-noscreen | capp-noscreen | package-noscreen   # 不带屏版各步骤
make clean                # 清理全部构建产物
```

**两个版本的区别**（编译期开关 `AKA_WITH_SCREEN`，构建目录与产物完全隔离、互不覆盖）：

| | 带屏版（默认） | 不带屏版 |
|---|---|---|
| 编译宏 | `-DAKA_WITH_SCREEN=1` | `-DAKA_WITH_SCREEN=0` |
| 二进制 | `bin/aka-capp` | `bin/aka-capp-noscreen` |
| 构建目录 | `build-cross/` | `build-cross-noscreen/` |
| 部署包 | `dist/aka-capp.tar.gz` | `dist-noscreen/aka-capp-noscreen.tar.gz` |
| 工具 | 含 `screen_test` | 不含 |
| 屏显示 | 摄像头画面 → /dev/fb0 | **整个显示栈不参与编译**（二进制里无 `/dev/fb0`，`[display]` 配置被忽略） |

给没有屏的机器用不带屏版：二进制更小、完全不碰 framebuffer，也彻底排除
屏相关代码对采集/服务的影响。整机行为其余部分完全一致。


说明：

- 交叉编译需要 riscv64-unknown-linux-musl 工具链（默认
  `/home/junbo_dai/riscv64-linux-musl-x86_64`，orb 内）；工具链路径可用
  `make -f Makefile.cross CXX=/path/to/riscv64-unknown-linux-musl-g++` 覆盖
- `capp` 目标带 FORCE：每次重跑交叉编译（内部增量，秒级），防止 `bin/aka-capp`
  被本机 host 构建误覆盖成非 RISC-V 二进制
- 本机开发调试构建用 `cd capp && make`，输出 `bin/aka-capp-dev`（macOS/Linux 版），
  与交叉产物 `bin/aka-capp` 互不干扰

## 部署（SG2002）

`make package` 打包出 `cpp/dist/AKA-00/`（部署目录）+ `cpp/dist/aka-capp.tar.gz`
（顶层 `AKA-00/` 目录，解压落到 `$AKA_HOME/`）。目标布局：

```
$AKA_HOME/
├── aka-capp                  # riscv64 静态二进制（3.5MB，无任何依赖）
├── config.toml               # 配置（见下）
├── static/                   # 前端构建产物（frontend → npm run build 产出）
├── arm_angles.json           # 机械臂角度（可选，缺省用默认值）
├── arm_angles_default.json   # 默认角度
├── speed_config.json         # 行驶速度配置
├── VERSION                   # 版本文件（OTA 用）
├── demo/<demo>/init.sh       # demo 目录（含 init.sh 的才会打包）
├── init.sh                   # 启动（自愈循环）
├── stop.sh                   # 停止
└── init_ap_web.sh            # AP 热点 + 开机自启配置（开机广播 AP，访问 192.168.4.1）
```

传到板子二选一：

```sh
# 方式 A：tar.gz（推荐，保留权限位，解压直接落到 $AKA_HOME/）
scp cpp/dist/aka-capp.tar.gz root@<板子IP>:~
ssh root@<板子IP> "cd ~ && tar -xzf aka-capp.tar.gz"

# 方式 B：整目录（scp -r 不保留可执行位，但 init.sh 有兜底 chmod）
scp -r cpp/dist/AKA-00 root@<板子IP>:~/AKA-00
```

> 注意：OpenSSH 的 `scp -r` 默认**不保留可执行位**，传完可能出现
> `aka-capp: Permission denied`。`init.sh` 启动前会兜底 `chmod +x`，tar 包则天然
> 保留权限 —— 优先用方式 A。若不用 init.sh 而直接跑二进制，先 `chmod +x aka-capp`。

启动（`init.sh`，进程崩溃自动重启；OTA 进行中自动等待）：

```sh
AKA_HOME=$HOME/AKA-00 $HOME/AKA-00/init.sh
```

### AP 热点 + 开机自启（`init_ap_web.sh`）

对应 Python 版 `init_ap_web.sh`。一次性运行后，板子**开机自动广播一个 AP 热点**
（`wlan0`，SSID 形如 `chenlong-robot-<MAC后6位>`，开放，IP `192.168.4.1`），
手机/控制器连上热点后浏览器访问 `http://192.168.4.1` 即可控制；`wlan1` 作为
STA，由 capp 的 `/api/wifi/scan`、`/api/wifi/connect` 扫描并连接目标路由器。

```sh
# 装配置 + 开机脚本（S98apstart / S99webstart），不改动当前网络 → reboot 后生效
$HOME/AKA-00/init_ap_web.sh install

# 或立即切换：wlan0 从 STA 切成 AP（会断开当前 WiFi 连接）
$HOME/AKA-00/init_ap_web.sh            # = install + 立即启动
```

> 适配点（与 Python 版的差异）：本板（SG2002 / HD05085A）无 `udhcpd`，改用
> `dnsmasq` 做 DHCP；只 `terminate wlan0` 的 wpa_supplicant，不 `killall`，避免
> 误杀 capp 在 `wlan1` 上自举的 wpa_supplicant；capp 在 `S99webstart` 里用
> `init.sh &` 后台启动而非 `exec`，避免 rcS 卡在 sysinit 导致 getty 不启动。
> 如需给热点加密码，取消 `/etc/hostapd.conf` 里 `wpa=2` 那 4 行的注释。

停止：`$AKA_HOME/stop.sh`（SIGTERM 优雅退出并停电机）。

环境变量：

| 变量 | 作用 | 默认 |
|---|---|---|
| `AKA_HOME` | 应用根目录（config.toml/static/VERSION/demo 等相对它） | `.` |
| `ARM_ANGLES_PATH` | 机械臂角度文件路径 | `$AKA_HOME/arm_angles.json` |
| `DEMO_BASE_DIR` | demo 目录 | `$AKA_HOME/demo` |
| `CSRC_LOG_LEVEL` | 日志级别 error/warn/info/debug | info |
| `STATUS_REPORT_URL` / `STATUS_REPORT_INTERVAL` | 云端状态上报地址 / 间隔秒 | config.toml / 300 |
| `OTA_CHECK_URL` | OTA 检查地址 | config.toml |
| `AKA_SERVER_NAME` | OTA 重启脚本要 kill 的进程名 | `aka-capp` |

### 配置（config.toml）

```toml
[camera]
width = 320
height = 240
fps = 24
jpeg_quality = 30

[motor]
backend = "tt_pid"      # "tt_pid" 真实硬件 / "dev" 开发 mock
port = "/dev/ttyS1"
baudrate = 115200
ppr = 4680

[arm]
backend = "zp10s"       # "zp10s" / "sts3215" / "dev"
port = "/dev/ttyS2"
baudrate = 115200

[web]
port = 80
https_port = 5443          # 0 = 关闭 HTTPS
https_cert = "cert.pem"    # 相对 $AKA_HOME 或绝对路径
https_key  = "key.pem"

[ota]
check_url = "https://api.chenlongrobot.com/api/user/robot-versions/featured"

[chassis]
wheel_diameter_mm = 62
gear_ratio = 90

[logging]
level = "info"
```

找不到 config.toml 时 motor/arm 全部走 mock（不控制硬件），Web 仍可启动，方便调试。

### 底盘自动重连（motor backend=tt_pid）

底盘 UART 不再作为服务启动的硬依赖（否则 ESP32 上电晚几百 ms 就会导致服务
起不来 / 静默降级 mock 后永远连不上）：

- 服务启动**不阻塞、不抛异常**：构造 `create_motor_pair` 即返回
  `AutoReconnectMotorPair` 代理，后台线程按退避策略（0.5s→1s→…→30s 封顶）
  持续尝试 INIT/CONFIG 握手，连上即自动切换为真实驱动；期间命令落到 mock。
- 已连接后每 ~1.5s 一次 `GET_STATUS` 心跳探活，连续 2 次失败判定掉线 →
  自动换回 mock 并重连（ESP32 意外重启/掉线可自愈）。
  心跳**不仅看"有应答"，还校验固件状态 ≥ READY**：ESP32 重启后处于
  UNINIT(0) 也会应答 GET_STATUS，但固件对速度命令要求 READY，未就绪照样
  判定掉线并自动重连（否则车不动，只能靠手动 reinitialize 才能恢复）。
- `reinitialize`（WS `{"type":"reinitialize"}`）：**已连上时原地重发
  INIT/CONFIG**（清 PID/编码器，不掉线不打断运行）；未连上或原地重发失败
  才断开并完整重连一次（自愈）。
- 状态**主动推送，前端无需轮询/刷新**：WS 建连即推一次
  `{"type":"motor_status","motor":{...}}`，此后仅在 connected/state 变化时推。
  `motor` 对象：`{backend, enabled, connected,
  state: "connected"|"reconnecting"|"disabled", attempts, error}`。
  控制页在底盘未连接时显示红色"自动重连中"徽标，连上自动消失。
  REST `/api/motor/status`、`/api/camera/all_status` 也带同名字段。
- **速度单位**：ESP32 固件返回的 rpm 已是轮速（编码器 4680 脉冲/轮圈、
  `PWM_RPM_MAX=150`，见 esp32_base_control/base_control.ino），线速度
  `m/s = rpm × π × 轮径(62mm) / 60`，**不要**再除齿轮比（旧版除 90 导致
  前端速度恒显示 0.0）。
- Python 版（`run.py` + Flask，对应 `src/base_control/auto_reconnect.py`）行为一致。

### HTTPS（`web.https_port`）

capp 同时支持 HTTP 和 HTTPS：默认 `:80` 与 `:5443` 共存（与原 Python `run.py` 行为一致，
前端开 `https://<板子IP>:5443` 自动升级 `wss://`）。TLS 终止走 mbedTLS（嵌入式库，
~300KB；首次 `make` 自动经 `cpp/scripts/build-mbedtls.sh` 交叉编译到
`third_party/mbedtls/`，链接进 `bin/aka-capp`）。

- 证书：capp **不**自己生成。`capp/scripts/init.sh` 启动前会调用打包里的
  `https_init.sh`（仓库根脚本），缺一即用 `openssl req -x509 -newkey rsa:4096 ...`
  生成自签 `cert.pem`/`key.pem` 到 `$AKA_HOME/`（10 年有效期）。
- 自签证书客户端会告警；AP 模式下手机连热点后浏览器点"高级 → 继续访问"即可。
  正式运营把 `cert.pem`/`key.pem` 换成 CA 签发的即可，无需改代码。
- `https_port = 0` 即关闭 HTTPS（HTTP 仍可用）；cert/key 缺失时 TLS 监听静默
  关闭，warn 一行不影响 HTTP 启动。

## API 契约（与前端 frontend/src/api.ts 完全对齐）

| 路由 | 说明 |
|---|---|
| `GET /api/control?action=..&speed=..&time=..&distance=..&angle=..` | 动作控制 |
| `GET /api/motor/status` `GET /api/motor/direct?left=&right=&duration=` `GET /api/motor/raw_command?cmd=` | 电机 |
| `GET/POST /api/arm/angles` `GET/POST /api/arm/angles/default` `POST /api/arm/angles/preview` | 机械臂 |
| `GET /api/camera/status` `POST /api/camera/open|close` `GET /api/camera/stream|snapshot|speed|all_status` | 摄像头 |
| `GET /api/display/status` `POST /api/display?enabled=&scale=&orient=&fps=&noise=` | 板载屏显示 |
| `GET /api/demo/list|name` `POST /api/demo/init|stop|download_model_with_progress|upload_model` `GET /api/demo/download_progress/{id}` | demo |
| `GET /api/ota/version|status|check|upgrade/progress` `POST /api/ota/upgrade|update` | OTA |
| `GET /api/system/info|ip|heartbeat` | 系统 |
| `GET /api/wifi/ip|status|scan` `POST /api/wifi/connect` | WiFi |
| `GET/POST /api/config/speed` | 速度配置 |
| `WS /ws/control` | 二进制控制通道（0xAA 摇杆 / 0xDD JSON / 0xBB 状态） |
| `GET /` 及 `/assets/*` | 前端静态文件（SPA fallback → index.html） |

## 板载屏显示（摄像头 → /dev/fb0）

`csrc::ScreenDisplay` 把摄像头画面实时显示到板载 SPI 屏（ST7796S 320x480 RGB565），
随 capp 启动（`[display] enabled = true`），也可用 `POST /api/display` 运行期开关/调参。

**硬件事实（板上实测，决定了参数选择）**

| 项 | 值 | 说明 |
|---|---|---|
| SPI 时钟 | 出厂 **4MHz** → 实测稳定上限 **20MHz** | 设备树 `st7796s@0/spi-max-frequency`；4MHz 时 ~420KB/s（约 2fps），20MHz 时 ~2MB/s |
| 24MHz 及以上 | ✗ 白屏 | 面板/走线信号完整性到顶；杜邦线转接会明显降低可跑频率 |
| 全屏写 | 307KB/帧 | 20MHz 下受带宽限制约 8fps |
| 半屏写（默认 scale=2） | 75KB/帧 | 可吃满摄像头 15fps |

**实现要点**

- **共享解码**（保证不拖慢浏览器）：`Camera::latest_rgb(max_out_w, ...)` 带缓存，
  同一帧只解码一次 —— 屏显示线程与 `/api/camera/stream` 的重编码路径共用结果，
  所以开屏后浏览器反而省掉自己那次整帧解码（640x360 MJPEG → 320 宽约 10ms/帧）。
  屏幕新增开销只有 RGB565 转换 + 脏行写屏（各几 ms）。
- **脏行检测**：逐行比较，只重写内容变化的行（SPI 屏按行扫描，静态区域零流量）；
  带噪声容差（`[display] noise`，忽略每通道 N 个 LSB），否则实况噪声会让"全行都变"。
- **整数查表转换**：RGB8→RGB565 + 旋转 90° + cover 缩放预计算成查表，
  riscv64 上避免逐像素浮点（否则慢一个量级）。
- **不干扰 Web 服务**：显示帧率上限 `[display] fps`，无新帧时零拷贝零解码，
  无 `/dev/fb0` 时自动跳过（不影响服务启动）。

**板上直测工具**（不起 Web 服务，排查屏/接线用）：

```sh
screen_test info            # framebuffer 信息 + 显示引擎状态
screen_test colors          # 纯色顺序播放（确认 RGB565 字节序/通道序/方向）
screen_test bench           # 写屏带宽基准（整块 vs 逐行）
screen_test camera [scale]  # 摄像头实时预览（默认 scale=2 半屏）
```

**相关配置**（`config.toml`）

| 键 | 默认 | 说明 |
|---|---|---|
| `[camera] width/height` | 640x360 | 须用原生可出流档；**320x240 是假档**（S_FMT 成功但不出帧） |
| `[camera] exp_fix` | false | 固定曝光/AWB/增益：自动控制抖动会让整幅画面每帧一起变、脏行检测失效（暗光下画面会偏暗）。等价于 demo 的 `DEMO_EXP_FIX=1` |
| `[display] enabled` | true | 随服务开屏（无 `/dev/fb0` 自动跳过） |
| `[display] scale` | 2 | 显示区域 = 屏幕 1/scale（2 → 160x240，吃满摄像头帧率） |
| `[display] orient` | 3 | 0无 1水平翻 2垂直翻 3=180°（本板实测 3 为正） |
| `[display] fps` | 15 | 显示帧率上限（与 `[camera] fps` 对齐） |
| `[display] noise` | 1 | 脏行容差：忽略每通道 N 个 LSB |
| `[display] decode_max_w` | 320 | 解码降采样上限宽；与 `[camera] stream_width` 一致可命中共享缓存 |

**运行期：`GET /api/display/status`、`POST /api/display?enabled=&scale=&orient=&fps=&noise=`。**

**开关联动**（默认 `[display] follow_camera = true`，屏跟随摄像头开关）

| 动作 | 屏行为 |
|---|---|
| 开机（摄像头默认关） | 清一次屏、保持黑，不出图 |
| 前端打开摄像头（`CameraToggle` / RC 页黑屏点击 → `POST /api/camera/open`） | 屏自动开始显示画面 |
| 前端关闭摄像头（`POST /api/camera/close`） | 屏清屏熄灭（不留最后一帧） |
| `GET /api/camera/status`、open/close 响应 | **不变**（屏状态对前端透明，仅 `/api/display/status` 可供运维查看） |
| `POST /api/display?enabled=1` 手动开屏 | 摄像头未开时顺带打开它（要画面就得有摄像头） |
| `[display] follow_camera = false` | 退化为开机常显（旧行为，会顺带打开摄像头） |

## 与原 Python 版本的差异（有意为之）

1. **摄像头采集**：V4L2 + libjpeg（参考 `tests/demo_camera.c` 思路），不依赖
   OpenCV。MJPEG 帧直通 `/api/camera/stream`；`/api/camera/snapshot` 返回摄像头原生
   尺寸（Python 版会 letterbox 到 320x240）。
2. **HTTP / WebSocket / JSON**：全部自研（POSIX socket + 线程），无第三方依赖，
   riscv64 musl 静态编译最简单。
3. **https**：服务端走 mbedTLS（HTTPS 监听 + TLS 终止，跨编译进 riscv64 musl 静态二进制）；
   客户端（`https://` 出栈请求：OTA 检查、状态上报）走 `curl -sS` 兜底，板上需装 curl。
   纯 `http://` 走内置 socket 客户端（demo 模型下载、OTA 固件下载）。
4. **摇杆换算**：WS joystick 用差速转向公式（左 = y+x，右 = y-x，±100 限幅），
   与前端 ControlSocket 契约一致。
5. **OTA 重启脚本**：进程名默认 `aka-capp`（`AKA_SERVER_NAME` 可覆盖），固件仍是
   `/tmp/aka-ota-update --update` 自解压脚本。
6. **状态换算**：RPM → m/s 用 config.toml `[chassis]`（wheel_diameter_mm=62,
   gear_ratio=90）。
7. **arm_angles.json 路径**：`$ARM_ANGLES_PATH` > `$AKA_HOME/arm_angles.json` > cwd。
