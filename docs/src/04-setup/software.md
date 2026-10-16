# 开发环境与部署

## 本机跑起来（不需要板子）

设备端是个普通的 C++ 程序（动作已内置进二进制），可以直接在开发机上编一份跑，配置、前端页面、模型/卡片
都用仓库里的实体文件：

```bash
# 1. 前端（改了前端才需要）
cd frontend && npm i && npm run build      # 产物写到仓库根 static/

# 2. 设备端（host 版，不会覆盖交叉编译产物）
cd cpp/capp && make                        # → bin/aka-capp-dev
AKA_HOME=cpp/board ./cpp/capp/bin/aka-capp-dev
```

`AKA_HOME` 一定要指向 `cpp/board`，否则找不到 `config.toml` 和 `static/`。
串口和摄像头拆掉也能启动（对应模块退化成 mock 并打日志），所以只写接口、调页面时
不用接硬件。

前端热更新：

```bash
cd frontend && npm run dev
```

> vite 的 dev server 把 `/api`、`/ws` 代理到 `localhost:5000`，而 C++ 版 capp 默认监听
> **80**（`config.toml` 的 `[web] port`）。要联调，要么改 `frontend/vite.config.ts` 里的
> proxy target，要么临时把 capp 的端口设成 5000。

## 打包

正式构建统一走 `cpp/Makefile`（在 orb 里直接跑；在 macOS 上会自动经 `orb run` 转发
交叉编译）：

```bash
cd cpp
make              # 一条龙：libjpeg/mbedtls → csrc → capp → package → ota
make noscreen     # 不带屏版本（整个显示栈编译期裁掉）
make clean        # 清理构建产物
```

产物两样：`cpp/dist/AKA-00/`（部署目录）与 `cpp/dist/aka-00-server`（自解压安装器）。

**前端不参与 make**：打包时直接拿仓库根 `static/` 里已有的产物，所以改了前端要先
`cd frontend && npm run build`（不带屏版是 `npm run build:noscreen`），拿错版本打包
脚本会告警。

## 部署到控制板

```bash
# 首次部署：解包 + 配热点 + 开机自启
scp -O cpp/dist/aka-00-server root@<板子IP>:/root/
ssh root@<板子IP> '/root/aka-00-server --init'

# 升级：保配置换包 + 重启服务
scp -O cpp/dist/aka-00-server root@<板子IP>:/root/
ssh root@<板子IP> 'setsid /root/aka-00-server --update >/root/ota.log 2>&1 < /dev/null'
```

- `scp` 加 `-O`（板载 sshd 不认新版 SFTP 协议）；固件别放 `/tmp`（那是 53MB 的内存盘，
  20MB 的固件会把内存打爆）。
- `--update` 默认**保留**板上的 `config.toml`、`speed_config.json`、`arm_angles.json`、
  `cert.pem`、`key.pem`，以及 demo 卡片（板上优先）和用户上传的模型（取并集）。
  想让包里带的新配置生效：`AKA_OTA_RESET_CONFIG=1 ... --update`。
- 换包用 staging + 目录改名，旧目录留成 `$AKA_HOME.old` 作回滚点；只解包不重启用
  `--extract`。
- `--update` 结尾是 `exec init.sh`，服务会挂在当前 ssh 会话上，所以要 `setsid`，
  或者升级完 `reboot`（开机由 `/etc/init.d/S99webstart` 拉起）。

更完整的说明见仓库根 `README.md` 的「📦 打包」与「🚀 部署」两节。
