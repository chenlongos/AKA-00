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
│   │   ├── motor_pair.hpp    MotorPair 接口 + 工厂（自动重连；**无 mock**，连不上即报错）
│   │   ├── zp10s.hpp         ZP10S 舵机驱动（对应 zl/zp10s/uart_control.py）
│   │   ├── sts3215.hpp       STS3215 总线舵机驱动（对应 sts3215/__init__.py）
│   │   ├── gripper.hpp       Gripper 接口 + 适配器 + 工厂（**无 mock**，接不上即报错）
│   │   ├── camera.hpp        V4L2 + libjpeg 摄像头（参考 tests/demo_camera.c）
│   │   ├── screen_display.hpp 摄像头画面 → 板载 SPI 屏（/dev/fb0，ST7796S 320x480）
│   │   ├── state.hpp         RobotStatus + StateCollector（对应 src/state/__init__.py）
│   │   ├── system_utils.hpp  IP / MAC / CPU / 内存 / 磁盘 / uptime
│   │   └── http_client.hpp   极简 HTTP 客户端（https 走 curl 兜底）
│   └── src/*.cpp（camera/、display/ 下按“设备 I/O / 图像换算”分文件）
├── capp/                     ← app/ 的 C++ 移植（独立 HTTP+WS 服务）
│   ├── include/capp/
│   │   ├── context.hpp       应用共享状态（服务单例 + demo/ota 状态）
│   │   ├── http_server.hpp   极简 HTTP 服务器（路由/CORS/静态文件/流式响应）
│   │   ├── websocket.hpp     RFC 6455 WebSocket（/ws/control 二进制协议）
│   │   └── routes.hpp
│   ├── src/
│   │   ├── main.cpp          入口（初始化硬件 → 启动 HTTP）
│   │   ├── routes.cpp        路由注册入口（按域调用 routes/ 下各文件）
│   │   ├── routes/           一域一文件（对照 app/routes/*.py）：motor/arm/camera/
│   │   │                     models/demo/display/ota/system/wifi/config/ws
│   │   ├── services/         服务层（对照 app/services/*.py）：control/camera/detect/
│   │   │                     demo_assets/display/status_reporter + waits/init
│   │   ├── http/             HTTP 层：conn（连接+TLS BIO）/ message（报文）/
│   │   │                     router（路由表）/ server（监听与请求循环）
│   │   └── websocket.cpp
│   └── Makefile / Makefile.cross（本机 dev / 交叉编译）
├── board/                    ← 板上目录的实体文件（打包时原样收进 dist/AKA-00/，见下）
│   ├── config.toml  init.sh  stop.sh  init_ap_web.sh  S97akaap  https_init.sh
│   ├── arm_angles*.json  speed_config.json  VERSION  start_img.jpg
│   └── demo/                 动作脚本（grab.lua / approach.lua）+ 模型（models/）+ 卡片（configs/）
├── scripts/                  build-libjpeg.sh / build-mbedtls.sh / build-lua.sh / build-ota.sh
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
  `frontend/` 里重新 build 再重新打包即可
- **前端也有"有没有屏"的编译开关**，与后端的 `AKA_WITH_SCREEN` 一一对应：

  | 命令 | 开关 | 界面差别 |
  |---|---|---|
  | `npm run build` | `WITH_SCREEN` 不设（=1） | 设置页有「屏幕显示」开关 |
  | `npm run build:noscreen` | `WITH_SCREEN=0` | **整块不存在**（不是显示了再隐藏） |

  **两个版本共用同一个产物目录 `static/`**，所以打包前 build 对版本：

  ```sh
  cd frontend && npm run build            && cd .. && make -C cpp ota              # 带屏
  cd frontend && npm run build:noscreen   && cd .. && make -C cpp ota-noscreen     # 不带屏
  ```

  拿错版本时打包脚本会报警（`static/assets/index.js` 里的 `__WITH_SCREEN__` 标记 ——
  带屏版才有；浏览器控制台里也能用它确认手上这个页面是哪版）。
- 页面与 capp 的接口契约（REST + WS 二进制协议）见下节

## 构建

### 用 make 构建（orb / macOS 通用）

顶层 `cpp/Makefile` 用 make 驱动整个构建。**在 orb 里直接 `make`**；在 macOS 上
`make` 会自动经 `orb run` 转发交叉编译（产物落在共享目录）：

```sh
cd cpp

# ── 带屏版本（默认）──
make                      # 全流程: libjpeg → mbedtls → csrc → capp(riscv64) → package → ota
make screen               # 只打包（= package，不含安装器）
make ota                  # 只生成自解压安装器（内部先 package）
# → 产物只有两样: dist/AKA-00/（部署目录）与 dist/aka-00-server（自解压安装器）
#   aka-00-server 同时用于首次部署（--init/--extract）与 OTA 升级（--update）

# ── 不带屏版本（整个显示栈编译期裁掉）──
make noscreen
# → 产物: capp/bin/aka-capp-noscreen、dist-noscreen/AKA-00/、
#   dist-noscreen/aka-00-server（自解压安装器，与带屏版是同一份脚本，只有 payload 不同）

make libjpeg              # 交叉编译 libjpeg（首次自动下载源码）
make mbedtls              # 交叉编译 mbedTLS（首次自动下载源码，HTTPS 用）
make csrc | capp | tools | package                      # 带屏版各步骤
make csrc-noscreen | capp-noscreen | package-noscreen | ota-noscreen   # 不带屏版各步骤
make clean                # 清理全部构建产物
```

**两个版本的区别**（编译期开关 `AKA_WITH_SCREEN`，构建目录与产物完全隔离、互不覆盖）：

| | 带屏版（默认） | 不带屏版 |
|---|---|---|
| 编译宏 | `-DAKA_WITH_SCREEN=1` | `-DAKA_WITH_SCREEN=0` |
| 二进制 | `bin/aka-capp` | `bin/aka-capp-noscreen` |
| 部署后的名字 | `aka-capp` | **也叫 `aka-capp`**（`init.sh` 写死了这个名字；带 `-noscreen` 后缀会起不来） |
| 构建目录 | `build-cross/` | `build-cross-noscreen/` |
| 部署产物 | `dist/AKA-00/` + `dist/aka-00-server` | `dist-noscreen/AKA-00/` + `dist-noscreen/aka-00-server`（**文件集合与带屏包完全相同**，只有 `aka-capp`、`tools/screen_test` 这两个二进制的内容不同） |
| 工具 | `tt_pid_test` / `cam_probe` / `screen_test` | **同一套**（`screen_test` 编出来是桩：能跑，但不碰屏） |
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
  与交叉产物 `bin/aka-capp` 互不干扰。跑的时候要指 AKA_HOME（配置/静态文件都在部署目录里）：
  `AKA_HOME=cpp/board ./cpp/capp/bin/aka-capp-dev`

### 部署内容 = `cpp/board/`（实体文件）

板上要什么由**实体文件**说了算：`cpp/board/` 就是 `$AKA_HOME/` 的镜像 ——
`config.toml`、`init.sh`/`stop.sh`/`init_ap_web.sh`/`S97akaap`、`https_init.sh`、
`arm_angles*.json`、`speed_config.json`、`VERSION`、`start_img.jpg`、
以及 `demo/`（动作脚本 + 模型 + 卡片配置）全部躺在那儿。想改板上哪个文件就直接改那里的实体文件，
不用碰构建脚本。

`make package` 只做两件事：

1. 把 `cpp/board/` 原样收进 `dist/AKA-00/`；
2. 补三样**构建产物**：`capp/bin/aka-capp`、仓库根的 `static/`、`csrc/<builddir>/` 下的 `tools/`。

两份包**只允许编译产物的内容不同**：那几个 ELF（`aka-capp`、`tools/*`）与前端 bundle
（`static/assets/index.js`）。文件集合、权限、以及其余一切文本文件（`config.toml`、
`*.sh`、`*.json`、`demo/*.lua`、`static/index.html`…）必须逐字节相同 —— 免得部署时
才发现“这个包少了张图”或者“两个包的脚本不一样”。

所以"这个文件到底哪来的"这类问题，答案只有两种：要么在 `cpp/board/` 里，
要么是编出来的（二进制 / 前端产物 / 板测工具）。

## 部署（SG2002）

`make -C cpp`（= package + ota）产出**两样东西**：`cpp/dist/AKA-00/`（部署目录，顶层就是
`AKA-00/`，解压落到 `$AKA_HOME/`）与 `cpp/dist/aka-00-server`（自解压安装器，首次部署与
OTA 升级共用）。目标布局：

```
$AKA_HOME/
├── aka-capp                  # riscv64 静态二进制（3.5MB，无任何依赖）
├── config.toml               # 配置（见下）
├── static/                   # 前端构建产物（带屏版 npm run build / 不带屏版 build:noscreen）
├── arm_angles.json           # 机械臂角度（可选，缺省用默认值）
├── arm_angles_default.json   # 默认角度
├── speed_config.json         # 行驶速度配置
├── VERSION                   # 版本文件（OTA 用）
├── demo/                     # demo 相关全在这一个目录下（仓库 cpp/board/demo/ 镜像过来）
│   ├── grab.lua              #   **动作脚本**（预定义、与模型无关，模型从 params.model 读）
│   ├── approach.lua          #   另一个动作：只接近瞄准、不夹取
│   ├── models/*.cvimodel     #   模型库
│   └── configs/<卡片名>.json  #   **卡片**：{"action":..,"model":..,+ 四个参数}（用户建的）
├── init.sh                   # 启动（自愈循环）
├── stop.sh                   # 停止
├── init_ap_web.sh            # AP 热点 + 开机自启配置（开机广播 AP，访问 192.168.4.1）
└── S97akaap                  # 首次开机自举 AP（镜像构建时拷到 /etc/init.d/）
```

> 这个目录里的东西（除 `aka-capp`、`static/`、`tools/` 三样构建产物外）**都在
> `cpp/board/` 有对应的实体文件**，见下面「部署内容 = `cpp/board/`」一节。

传到板子二选一：

```sh
# 方式 A：自解压安装器（推荐；权限位自带，换包是"staging + 目录改名"，失败有 .old 回滚点）
scp cpp/dist/aka-00-server root@<板子IP>:/tmp/
ssh root@<板子IP> 'chmod +x /tmp/aka-00-server && /tmp/aka-00-server --update'
#   ⚠ --update 默认**保留**板上的 config.toml / speed_config.json / arm_angles.json：
#     想让本次带的默认配置（如 [display] scale=1 全屏）生效，要么
#     AKA_OTA_RESET_CONFIG=1 /tmp/aka-00-server --update，要么部署后手改 config.toml。
#   首次部署用 --init；只解包不重启用 --extract。

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

**连过的 WiFi 会被记住**：连接成功时把 `{ssid, password}` 写进 **`/etc/aka-wifi.json`**
（`0600`，原子写），capp 每次启动在**后台线程**里重放一次 —— 用户不用每次开机都重连。
只保留**最后一个**（换网就覆盖）。实现见 `cpp/capp/src/services/wifi_service.cpp`。

> `/etc/aka-wifi.json` 是**第一个由 capp 自己生成、不在部署体系内**的持久文件，所以
> 单独说明它为什么在那儿：
>
> - **不放 `$AKA_HOME`**：OTA 的 `--update` 是 `swap_in` 整目录换包，`$AKA_HOME` 里
>   不进 `KEEP_FILES` 的东西一律丢（而且那份白名单用 `[ -s ]` 判空，**0 字节文件会被
>   静默扔掉**）。放 `/etc` 就不在 `swap_in` 的范围内，升级天然保留。
> - **不放 `/root`**（虽然那把 AP 的锁在那儿）：两边性质不同 —— 锁是"装过没装过"的
>   标记，这个是用户数据，跟 AP 配置（`/etc/hostapd.conf`、`/etc/dnsmasq.ap.conf`）
>   同处一地更自然。
> - 想"忘记网络"：`rm /etc/aka-wifi.json` 即可（下个版本可能加个接口，现在就这样）。

职责是分开的，别混：

| 文件 | 职责 |
|---|---|
| `S97akaap` | **判断"装过没装过"（锁）+ 决定调不调 + 装完上锁**。AP 的唯一入口 |
| `init_ap_web.sh` | **无状态的安装器**：谁调都装，不判断、不上锁 |

```sh
# 无状态：无条件安装（会断开 wlan0 当前 STA 连接）
$HOME/AKA-00/init_ap_web.sh             # = install + 立即启动 AP
$HOME/AKA-00/init_ap_web.sh install     # 只装配置和开机脚本，不动当前网络
$HOME/AKA-00/init_ap_web.sh start       # 只立即启动 AP

# 走锁：装过就跳过（开机由 rcS 调它；--init 走的是上面那条，不过锁）
$HOME/AKA-00/S97akaap
```

> 适配点（与 Python 版的差异）：本板（SG2002 / HD05085A）无 `udhcpd`，改用
> `dnsmasq` 做 DHCP；只 `terminate wlan0` 的 wpa_supplicant，不 `killall`，避免
> 误杀 capp 在 `wlan1` 上自举的 wpa_supplicant；capp 在 `S99webstart` 里用
> `init.sh &` 后台启动而非 `exec`，避免 rcS 卡在 sysinit 导致 getty 不启动。
> 如需给热点加密码，取消 `/etc/hostapd.conf` 里 `wpa=2` 那 4 行的注释。

#### `S97akaap`：开机自举 + 幂等锁

AP 的配置和 `S98apstart`/`S99webstart` **都是 `init_ap_web.sh` 生成的**。板子上从没
跑过它时，这些文件一个都没有 → 开机没有任何脚本会去调用它 → 死锁。所以有个
`S97akaap`，由**镜像构建拷到 `/etc/init.d/S97akaap`**：

```sh
# 镜像构建时（AKA-00 已用 --extract 展开在镜像里）
cp $IMAGE/root/AKA-00/S97akaap $IMAGE/etc/init.d/S97akaap && chmod 755 $IMAGE/etc/init.d/S97akaap
```

它是**开机那条路的入口 + 锁的持有者**，逻辑就三步：**有锁 → 跳过；没锁 → 调安装
脚本；装完产物齐全才上锁。**

> **和 `aka-00-server --init` 是两条独立的路**：`--init` 仍直接调 `init_ap_web.sh`
> （原版行为，没改）→ 装完**不上锁**，之后第一次开机 S97 会再装一遍才上锁。
> 这是有意的取舍：`--init` 是操作者显式动作，不该被锁拦住；锁只管开机。
> 走"镜像预烤"路线的话根本不跑 `--init`，不存在这次重复。

锁是 **`/root/.aka-ap-provisioned`** —— 放在 **AKA-00 的上一级**，两个理由：

- **OTA 换不到它**：`--update` 是 `swap_in` 整目录替换 `$AKA_HOME`（板上
  `/root/AKA-00`）。锁若在 AKA-00 里面，每次升级都会被冲掉 → 升级后开机重装 AP、
  手改的 `hostapd.conf` 被覆盖。
- **删起来顺手**：登 root 进去 `ls -a` 就看见，`rm -f /root/.aka-ap-provisioned`。

> 位置本身**不构成**安全依据（`/root` 跟 `/etc` 里的产物不在一个生命周期上）。
> 兜底的是产物检查 —— 见下面那条。

下面任一情况都会让锁失效并重装：

- **参数指纹变了** —— `AP_IFACE` / `AP_IP` / `NETMASK` / `DHCP_START` / `DHCP_END` /
  `CHANNEL` / `AKA_HOME`（参数在 `S97akaap` 里定义，由它显式传给安装脚本）
- **产物缺了或空了** —— `hostapd.conf`、`dnsmasq.ap.conf`、`S98apstart`、`S99webstart`
  少一个、或者哪个是 **0 字节**（`-s` 而不是 `-f`/`-x`：空文件能骗过后者，然后
  hostapd 拿着空配置起不来，板子静默没 AP）

**"有锁"严格蕴含"配置可用"**：锁是在调完安装脚本、确认产物齐全非空之后才写的 ——
判据是产物本身，不是安装脚本的退出码（它没有 `set -e`，失败也多半返回 0）。所以
半途断电/装失败留下的残局，下次开机会自动重来。

```sh
rm -f /root/.aka-ap-provisioned && $HOME/AKA-00/S97akaap   # 删锁重装
$HOME/AKA-00/init_ap_web.sh                              # 绕过锁，无条件重装
```

排序 `S97 < S98apstart < S99webstart`：

- **第一次开机**（没锁 → 真装）：S97 装好配置、当场拉起 AP（安装脚本第 6 节），
  然后**补跑一次 `/etc/init.d/S99webstart`**。
- **之后每次开机**：有锁，几毫秒 no-op；S98/S99 由 rcS 按正常顺序跑到。

> 为什么要补跑那一下：rcS 是 `for i in /etc/init.d/S??*`，**glob 在循环开始时就展开
> 完了** —— S97 刚写出来的 S98apstart / S99webstart 这一轮不会被跑到。不补的话第一次
> 开机就只有热点：**没有 `wlan1`**（S99 负责 `iw phy phy0 interface add wlan1`）、
> **也没有 capp**（S99 负责拉起 `init.sh`），用户连上热点打不开页面，得再重启一次。
>
> 现象（实测）：`/tmp/aka-ap-init.log` 里有 `[S97] AP 配置完成，已上锁`、
> `hostapd`/`dnsmasq` 在跑、`wlan0` 是 `192.168.4.1`，但 `ps` 里**没有 `aka-capp`**、
> `ip a` 里**没有 `wlan1`**。现场救急就是在串口敲一次 `/etc/init.d/S99webstart`。

日志：`/tmp/aka-ap-init.log`（`tee`，所以串口/终端上也看得到）。

> eth0 默认路由那件事（`/etc/network/interfaces` 里的静态 gateway 会压住 WiFi 的
> 默认路由）**不归 S97 管** —— 那是 `init.sh` 的活，见下面「eth0 默认路由」。

> **镜像里不要烤 `hostapd.conf` / `dnsmasq.ap.conf` / 锁。** SSID 是
> `chenlong-robot-<MAC后6位>` —— 每块板子不同。构建机上生成一次烤进去，全批次板子
> 就成了同一个 SSID，还会顶掉各自的配置。镜像只放展开好的 AKA-00 + `S97akaap`。
>
> 本脚本用**临时文件 + `mv`** 原子替换来写开机脚本（见 `install_boot_script`）：
> `cat >` 会先把目标截成 0 字节，写到一半断电就留下一个**空的开机脚本**（可执行但
> 什么都不干，开机静默没 AP）。

#### 已知问题：eth0 的默认路由（**未修**）

`/etc/network/interfaces` 给 eth0 配了静态 gateway（板上是 `192.168.1.1`），开机
`ifup` 就装一条 `default via 192.168.1.1 dev eth0`，**压住 wlan1（WiFi）那条默认
路由**（内核按 metric 选，先加的先赢）→ 板子出不去网，得手敲
`ip route del default via 192.168.1.1 dev eth0` 才行。

**当前代码没解决这个**（S97 不管，`init.sh` 里那段也不管用）。两条路都有问题：

1. **`init.sh:80` 用的是 `ip route show default dev eth0`，在 HD05085A 上不起过滤作用。**
   2026-09-22 实测：那个 `default` 关键字**被忽略**，它把 eth0 上所有路由都吐出来，
   于是循环连 `192.168.1.0/24 dev eth0 scope link` 这种**子网路由**一起删了 —— eth0
   当场不通（现象：`192.168.1.123` ping 不通）。
2. **而且它只在 `init.sh` 启动那一刻跑一次，还要求那时 `wlan1` 已经有 IP。** 典型用法是
   **开机之后在界面上连 WiFi**（`wlan1` 由 capp 自举，连上是后来的事）—— 那一刻条件
   不成立，之后再没人删。

**根治的位置是镜像**：构建时直接把 `/etc/network/interfaces` 里 eth0 的 `gateway`
注释掉（那本来就是固件的网络配置，不该由运行时脚本去改），坏路由从源头就不产生。
在改之前，这条得手敲。

停止：`$AKA_HOME/stop.sh`（SIGTERM 优雅退出并停电机）。

环境变量：

| 变量 | 作用 | 默认 |
|---|---|---|
| `AKA_HOME` | 应用根目录（config.toml/static/VERSION/demo 等相对它） | `.` |
| `ARM_ANGLES_PATH` | 机械臂角度文件路径 | `$AKA_HOME/arm_angles.json` |
| `CSRC_LOG_LEVEL` | 日志级别 error/warn/info/debug | info |
| `STATUS_REPORT_URL` / `STATUS_REPORT_INTERVAL` | 云端状态上报地址 / 间隔秒 | config.toml / 300 |
| `OTA_CHECK_URL` | OTA 检查地址 | config.toml |
| `AKA_SERVER_NAME` | OTA 重启脚本要 kill 的进程名 | `aka-capp` |

### 配置（config.toml）

```toml
[camera]
width = 640          # 必须用原生可出流的档（320x240 是假档，勿用）
height = 360
fps = 15
jpeg_quality = 75

[motor]
backend = "tt_pid"      # 只认 "tt_pid"（mock 已删除：连不上会明确报错）
port = "/dev/ttyS1"
baudrate = 115200
ppr = 4680

[arm]
backend = "zp10s"       # "zp10s" / "sts3215"（mock 已删除）
port = "/dev/ttyS2"
baudrate = 115200

[web]
port = 80
https_port = 443           # 0 = 关闭 HTTPS
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

找不到 config.toml 时按**默认值**跑（motor=tt_pid `/dev/ttyS1`、arm=zp10s `/dev/ttyS2`），仍然去连真硬件；连不上只打 ERROR、不假装能动，Web 照常启动。

### 底盘自动重连（motor backend=tt_pid）

底盘 UART 不再作为服务启动的硬依赖（否则 ESP32 上电晚几百 ms 就会导致服务
起不来 / 驱动永久不可用）：

- 服务启动**不阻塞、不抛异常**：构造 `create_motor_pair` 即返回
  `AutoReconnectMotorPair` 代理，后台线程按退避策略（0.5s→1s→…→30s 封顶）
  持续尝试 INIT/CONFIG 握手，连上即自动切换为真实驱动；**没连上期间指令被丢弃**（`active_` 为空），日志打 ERROR、`/api/motor/status` 报 `connected=false`。
- 已连接后每 ~1.5s 一次 `GET_STATUS` 心跳探活，连续 2 次失败判定掉线 →
  自动断开真实链路（`active_` 置空）并按退避重连（ESP32 意外重启/掉线可自愈）。
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

capp 同时支持 HTTP 和 HTTPS：默认 `:80` 与 `:443` 共存。443 是浏览器默认端口，所以
前端直接开 `https://<板子IP>/` 即可（前端用 `location.host` 拼 `wss://`，不带端口就是 443，
不用手输端口）。TLS 终止走 mbedTLS（嵌入式库，
~300KB；首次 `make` 自动经 `cpp/scripts/build-mbedtls.sh` 交叉编译到
`third_party/mbedtls/`，链接进 `bin/aka-capp`）。

- 证书：capp **不**自己生成。`init.sh`（`cpp/board/init.sh`）启动前会调用同目录的
  `https_init.sh`，缺一即用 `openssl req -x509 -newkey ec ...`
  （prime256v1，1 秒；原 RSA-4096 实测要 64 秒且卡在 capp 启动前）生成自签
  `cert.pem`/`key.pem` 到 `$AKA_HOME/`（10 年有效期）。
  证书属于现场数据：OTA 换包时由 `build-ota.sh` 的 `KEEP_FILES` 保留，不会每次重签。
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
| `GET /api/detect?model=<名字>&conf=&iou=` | 单帧推理：取当前帧跑一次模型，只回框的四个角（原像素坐标）。模型必填、裸名字映射 `demo/models/<名字>.cvimodel`；`conf`/`iou` 可选（默认 0.25 / 0.45） |
| `POST /api/models/upload?name=<名字>` `POST /api/models/delete` | 模型上传/删除：平台把模型文件推到 `demo/models/`（body 为文件；同名覆盖、覆盖即生效）；删除只删文件，用到它的卡片变成"模型缺失"（重传同名即复活） |
| `POST /api/demo/run` `GET /api/demo/status` `POST /api/demo/stop` | 跑**动作脚本**（`demo/grab.lua`、`demo/approach.lua`；模型用 `params.model` 传）。安全兜底（限速/被接管/掉线/内存）在宿主里；执行方式 `mode=once|loop`：**once 最多 5 分钟**（到点宿主自己收工），loop 没有总时长上限 |
| `GET /api/demo/list|name|config` `POST /api/demo/init|stop|config|delete` | demo 卡片 = **动作 × 模型**（用户建，一份配置一张卡 `demo/configs/<卡片名>.json`）。init 两种形状：`{"name":卡片名}` 或 `{"action":..,"model":..}` |
| `GET /api/ota/version|status|check|upgrade/progress` `POST /api/ota/upgrade|update` | OTA |
| `GET /api/system/info|ip|heartbeat` | 系统 |
| `GET /api/wifi/ip|status|scan` `POST /api/wifi/connect` | WiFi |
| `GET/POST /api/config/speed` | 速度配置 |
| `WS /ws/control` | 二进制控制通道（0xAA 摇杆 / 0xDD JSON / 0xBB 状态） |
| `GET /` 及 `/assets/*` | 前端静态文件（SPA fallback → index.html） |

## 板载屏显示（摄像头 → /dev/fb0）

`csrc::ScreenDisplay` 把摄像头画面实时显示到板载 SPI 屏（ST7796S 320x480 RGB565），
随 capp 启动（`[display] enabled = true`）。**屏没有任何 HTTP 接口**：开关与参数只在 `config.toml` 的 `[display]` 里配，
改完重启 capp 生效；屏状态对前端透明。

**硬件事实（板上实测，决定了参数选择）**

| 项 | 值 | 说明 |
|---|---|---|
| SPI 时钟 | 出厂 **4MHz** → 实测稳定上限 **20MHz** | 设备树 `st7796s@0/spi-max-frequency`；4MHz 时 ~420KB/s（约 2fps），20MHz 时 ~2MB/s |
| 24MHz 及以上 | ✗ 白屏 | 面板/走线信号完整性到顶；杜邦线转接会明显降低可跑频率 |
| 全屏写（**默认**） | 307KB/帧 | 写满整屏约 8fps（SPI 实际 23.44MHz ≈ 2.4MB/s 的物理上限） |
| 半屏写（`scale=2`） | 75KB/帧 | 可吃满摄像头 15fps；嫌全屏掉帧就改回 2 |

**实现要点**

- **共享解码**（保证不拖慢浏览器）：`Camera::latest_rgb(max_out_w, ...)` 带缓存，
  同一帧只解码一次 —— 屏显示线程与 `/api/camera/stream` 的重编码路径共用结果，
  所以开屏后浏览器反而省掉自己那次整帧解码（640x360 MJPEG → 320 宽约 10ms/帧）。
  屏幕新增开销只有 RGB565 转换 + 脏行写屏（各几 ms）。
- **脏行检测**：逐行比较，只重写内容变化的行（SPI 屏按行扫描，静态区域零流量）；
  比较时忽略 RGB565 每通道最低 1 bit（代码常量 `kDirtyMask`）—— 实况有传感器噪声，
  精确比较会让静止画面也判成"全行都变"（板上实测 240/240 行全脏）。
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
| `[display] scale` | **1（全屏 320x480）** | 显示区域 = 屏幕 1/scale；铺满用 cover 裁切（不拉伸变形，全屏会裁掉约 16% 上下边）；`2` = 半屏 160x240 |
| `[display] orient` | 3 | 0无 1水平翻 2垂直翻 3=180°（本板实测 3 为正） |
| `[display] fps` | **8** | 显示帧率上限：全屏一帧 307KB，SPI 实际 23.44MHz → 写满整屏上限约 8fps，再高只会让内核推送队列积压（延迟变大而非更流畅）；改回半屏可提到 15 |
| `[display] decode_max_w` | **640（全屏）** | 解码降采样上限宽。全屏要铺满 320x480，按 640 解码才清晰（按 320 会被放大 1.78 倍发虚）；半屏时用 320 更省 CPU 且与浏览器共享同一次解码 |

**看屏状态**：用 `CSRC_LOG_LEVEL=debug` 启动 capp，日志每秒一行 `fps | dec | conv | blit | 脏行数`；
启动时还有 `[display] ▶ 320x480 region …` 与 `首帧已上屏 …`（都是排障够用的信息，不需要额外接口）。

**熄屏待机图（`start_img.jpg`）**

摄像头关着的时候屏上不再是黑的，而是显示一张待机图；打开摄像头立刻清屏切实时画面，
关摄像头又回到这张图：

| 时机 | 屏上内容 |
|---|---|
| 开机（摄像头默认关） | 待机图（`[display] standby_image`，默认 `start_img.jpg`） |
| 打开摄像头（`POST /api/camera/open` / 前端开关） | 先清一次屏，再实时出图 |
| 关闭摄像头（`POST /api/camera/close`） | 待机图（替代原来的黑屏） |
| `[display] enabled = false`（config，重启生效） | 屏不参与显示，摄像头画面只走 Web |
| 图片缺失 / 解码失败 / 无 `/dev/fb0` | 退化为原来的清黑，不影响服务启动 |

- 图走**和摄像头画面完全同一套变换**（90° 旋转 + cover 缩放居中裁切 + `[display] orient`）。
  本板 `orient=3` 是照着"摄像头画面正立"调出来的，所以待机图也正立 —— 两者方向一致才合理，
  千万别只给待机图单独调方向。
- 取样用**盒式平均**（照片缩小时比最近邻干净），只在切图那一次跑；摄像头热路径仍是原来的
  最近邻查表，不受影响。
- 图与面板同比例最好：现用的 `start_img.jpg` 是 **480x320**（3:2），旋转后正好 320x480，
  解码走 libjpeg 1/N 档（上限是代码常量 kStandbyDecodeMaxW=1024，480 宽即原生 1:1 解码、不裁不补），
  一次几十毫秒，只在切图时发生。

| 配置键 | 默认 | 说明 |
|---|---|---|
| `[display] standby_image` | `start_img.jpg` | 相对路径按 `$AKA_HOME` 解析；留空 = 黑屏（旧行为） |

待机图**总是铺满整屏**（不受 `[display] scale` 影响）；解码宽度上限是代码里的常量，
不再作为配置项。

打包：`start_img.jpg` 两个版本的部署目录都带（不带屏版用不到它，但两份包的文件集合保持一致更重要）。
开发机单测（不需要板子）：`make -C cpp/csrc test-standby`，覆盖旋转方向、orient 四个翻转、
cover 裁切、盒式平均效果、越界写与异常输入，共 21 项断言。

**开关联动**（屏跟随摄像头开关，固定行为、无开关配置项）

| 动作 | 屏行为 |
|---|---|
| 开机（摄像头默认关） | 清一次屏、保持黑，不出图 |
| 前端打开摄像头（`CameraToggle` / RC 页黑屏点击 → `POST /api/camera/open`） | 屏自动开始显示画面 |
| 前端关闭摄像头（`POST /api/camera/close`） | 屏清屏熄灭（不留最后一帧） |
| `GET /api/camera/status`、open/close 响应 | **不变**（屏状态对前端透明，不暴露任何屏接口） |

**网页看摄像头不卡的推荐组合（实测结论：640 采集 + 直出）**

| 配置 | 值 | 作用 |
|---|---|---|
| `[camera] width/height` | `640x360`（**勿用 320x240**） | 摄像头**硬件压缩**出 640 MJPEG；320x240 是假档（不出帧） |
| `[camera] stream_scale` | `false`（默认） | 浏览器**直通原帧**：零解码零编码（省 ~25ms/帧） |
| `[camera] jpeg_quality` | 视带宽调（如 50） | 嫌带宽大就压摄像头侧质量，仍零 CPU |
| `[display] fps_streaming` | **`3`**（0=暂停） | 有人看流时屏显示降帧，浏览器优先（全屏写屏很吃 CPU/带宽，所以比半屏时降得更狠） |
| `[camera] exp_fix` | `true`（可选） | 画面稳定 → 脏行命中 → 屏写屏量大幅下降 |

**为什么"640 + 直出"最流畅**：压缩是摄像头硬件做的，服务端一个像素都不碰 ——
浏览器路径只剩"收帧 + 写 socket"，CPU 占用趋近 0。反过来，任何服务端处理
（缩放/重编码）都要在单核 C906 上多花 15~25ms/帧，直接体现为网页卡顿。
直出的额外好处是浏览器拿到的是 **640x360 原画质**（比缩到 320x180 更清晰）。

**浏览器优先（CPU 竞争）**：单核 SoC 上"屏显示 + 浏览器取流"同时跑会 CPU 饱和
（显示每帧 RGB565 转换 + SPI 写屏约 20ms，15fps ≈ 30% 单核；驱动推屏还有内核侧开销），
会明显拖慢网页看摄像头的帧率与延迟。因此 `/api/camera/stream` 有客户端连接时会把
显示线程降到 `[display] fps_streaming`（默认 3；**设为 0 = 有人看流时完全暂停屏显示**），
断开后自动恢复 `[display] fps`。仍嫌不够时用 `[display] scale` 缩小显示区域
（显示区 = 屏幕 1/scale：scale=2 是 1/4 屏，scale=3 约 1/9 屏）。

## 与原 Python 版本的差异（有意为之）

1. **摄像头采集**：V4L2 + libjpeg（参考 `tests/demo_camera.c` 思路），不依赖
   OpenCV。MJPEG 帧直通 `/api/camera/stream`；`/api/camera/snapshot` 返回摄像头原生
   尺寸（Python 版会 letterbox 到 320x240）。
2. **HTTP / WebSocket / JSON**：全部自研（POSIX socket + 线程），无第三方依赖，
   riscv64 musl 静态编译最简单。
3. **https**：服务端走 mbedTLS（HTTPS 监听 + TLS 终止，跨编译进 riscv64 musl 静态二进制）；
   客户端（`https://` 出栈请求：OTA 检查、状态上报）走 `curl -sS` 兜底，板上需装 curl。
   纯 `http://` 走内置 socket 客户端（OTA 固件下载）。
4. **摇杆换算**：WS joystick 用差速转向公式（左 = y+x，右 = y-x，±100 限幅），
   与前端 ControlSocket 契约一致。
5. **OTA 重启脚本**：进程名默认 `aka-capp`（`AKA_SERVER_NAME` 可覆盖），固件仍是
   `/tmp/aka-ota-update --update` 自解压脚本。
6. **状态换算**：RPM → m/s 用 config.toml `[chassis]`（wheel_diameter_mm=62,
   gear_ratio=90）。
7. **arm_angles.json 路径**：`$ARM_ANGLES_PATH` > `$AKA_HOME/arm_angles.json` > cwd。
