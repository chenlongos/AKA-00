# API 文档

## 控制接口

```
GET /api/control?action=<action>&speed=<speed>&time=<time>&distance=<distance>&angle=<angle>
```

### 参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|
| action | string | 是 | up / down / left / right / stop / grab / release |
| speed | int | 否 | 电机百分比（1~100），默认 50 |
| time | int | 否 | 持续时间（毫秒），无 distance/angle 时生效 |
| distance | float | 否 | **移动距离（厘米 cm）**，up/down 有效 |
| angle | float | 否 | **转动角度（度 °）**，left/right 有效 |

> **优先级**：`distance`/`angle` > `time`。传了 distance 或 angle 就忽略 time。
>
> `speed` 是直接发给 ESP32 PID 控制器的目标百分比（`setMotorSpeed(±100)` → `target_rpm = speed × 150 / 100`）。`100%` 对应约 `0.49 m/s`，由 `PWM_RPM_MAX=150 RPM × 轮径62mm × π / 60` 推出。

### 距离运动示例

```bash
# 前进 30 厘米，速度 50%
curl "http://<ip>/api/control?action=up&distance=30&speed=50"

# 后退 15 厘米，速度 30%
curl "http://<ip>/api/control?action=down&distance=15&speed=30"

# 左转 90 度，速度 40%
curl "http://<ip>/api/control?action=left&angle=90&speed=40"

# 右转 45 度（用默认 speed=50）
curl "http://<ip>/api/control?action=right&angle=45"
```

### 时间运动示例

```bash
# 前进 2 秒，速度 50%
curl "http://<ip>/api/control?action=up&speed=50&time=2000"

# 停止
curl "http://<ip>/api/control?action=stop"
```

### 抓取

```bash
curl "http://<ip>/api/control?action=grab"
curl "http://<ip>/api/control?action=release"
```

### speed 物理含义对照

`speed` 是 ESP32 PID 控制器的目标百分比。所有路径（摇杆 / 方向键 / REST+time / REST+distance）共用同一套物理含义：

| speed | 目标 RPM | 约合线速度 |
|-------|---------|-----------|
| 30 | 45 | 0.15 m/s |
| 50 | 75 | 0.24 m/s |
| 100 | 150 | 0.49 m/s |

---

## 电机直接控制

```
GET /api/motor/direct?left=<left>&right=<right>&duration=<duration>
```

| 参数 | 类型 | 说明 |
|------|------|------|
| left | int | 左轮 -100~100 |
| right | int | 右轮 -100~100 |
| duration | float | 持续时间（秒），0 为持续 |

```bash
# 全速前进
curl "http://<ip>/api/motor/direct?left=100&right=100"

# 原地右转
curl "http://<ip>/api/motor/direct?left=50&right=-50"

# 前进 1.5 秒
curl "http://<ip>/api/motor/direct?left=80&right=80&duration=1.5"
```

---

## 电机状态

```
GET /api/motor/status
```

```json
{
  "left_speed": 0.0,
  "right_speed": 0.0,
  "left_target": 50,
  "right_target": 50,
  "gripper_status": "stopped"
}
```

---

## 速度配置

```
GET /api/config/speed
POST /api/config/speed
```

```json
{"forward_speed": 50, "turn_speed": 50}
```

---

## 摄像头

### 状态

```
GET /api/camera/status
```

```json
{"camera_on": true}
```

### 打开 / 关闭

```
POST /api/camera/open
POST /api/camera/close
```

```json
{"camera_on": true}     // open 成功；打不开返回 500
{"camera_on": false}    // close
```

> 摄像头是**全局唯一**的一份：屏显示、浏览器取流、单帧推理共用它。
> 关闭会同时熄屏（屏上显示待机图），对前端透明；打开后屏自动实时出图。

### 抓拍（单张图）

```
GET /api/camera/snapshot
```

```json
{
  "image": "<base64 JPEG>",
  "width": 640,
  "height": 360,
  "format": "jpeg",
  "m": 2671.82,
  "c": -2.82
}
```

| 字段 | 说明 |
|------|------|
| image | 整帧图片的 base64（JPEG） |
| width / height | 图片像素尺寸 |
| format | 固定为 `jpeg` |
| m / c | 距离标定常数：`D = m / P + c`（`P` = 目标在画面中的像素尺寸，`D` = 距离）。配合检测框用，见[距离标定](dist-calibration.md) |

> 摄像头没开会**自动打开**（注意有副作用：`camera_on` 变成 true、板载屏开始实时出图），所以这里
> 拿到的是**实时帧**。打不开时（设备被占用/不存在）返回 `500` + `{"error":"camera not available"}`。
>
> MJPEG 摄像头是**原帧直通**（不重新编码，quality 参数对它无效）；只有 YUYV 摄像头才编码，quality=70。

### 视频流（MJPEG）

```
GET /api/camera/stream?fps=<fps>
```

| 参数 | 类型 | 说明 |
|------|------|------|
| fps | int | 发送帧率上限，默认 15，超出 1~30 会被夹到边界 |

响应为 `multipart/x-mixed-replace; boundary=frame` 的 MJPEG 流（浏览器 `<img src>` 直接用），持续到客户端断开。

> 默认直通摄像头原始 MJPEG 帧（服务端零解码零编码）；只有配置了 `[camera] stream_scale = true`
> 才会在服务端缩放到 `stream_width x stream_height` 后重编码下发 —— 那是拿 CPU 换 WiFi 带宽。
>
> 有人在看流时，板载屏会自动降帧到 `[display] fps_streaming`（0 = 暂停显示）让出 CPU 给浏览器：
> 单核 SoC 上"全屏写屏 + 浏览器取流"会互相拖慢，所以默认浏览器优先。

### 底盘速度 / 综合状态

两个**历史命名**的接口，回的实际是底盘与电机状态（不是摄像头信息），保留是为了兼容前端：

```
GET /api/camera/speed
GET /api/camera/all_status?timestamp=<timestamp>
```

```json
{
  "left_speed": 0.0,
  "right_speed": 0.0,
  "left_target": 50,
  "right_target": 50,
  "gripper_status": "stopped",
  "gripper_target": 0,
  "timestamp_ms": 1730000000000
}
```

`all_status` 在此之上多三个字段：`motor`（电机连接状态）、`image`（base64 JPEG，quality=25）、
`image_format`；传给它的 `timestamp` 会原样回显。`gripper_status` 是夹爪运行状态，夹爪未连接时是 `unknown`。

> **这个接口不会打开摄像头**（与 `snapshot` 不同）：摄像头关着时它读的是内存里缓存的最后一帧，
> 因此 `image` 依然是关闭前那一张、HTTP 依然 200，而 `camera_on` 为 `false`。实测：关闭后连续两次
> 取图，图片字节完全相同。响应里没有帧时间戳，要判断实时性只能看 `camera_on`。
> 从来没出过帧时 `image` 为 `null`。

---

## 单帧推理（物体检测）

```
GET /api/detect?model=<模型名>
```

取当前摄像头的一帧跑一次模型，返回检测框的四个角。

### 参数

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| model | string | 是 | 模型名，对应 `$AKA_HOME/demo/models/<模型名>.cvimodel`（如 `tennis`、`block`）。只允许字母数字与 `_ - .`，不允许 `/` 与 `..` |

> 摄像头没开会自动打开（与 `/api/camera/snapshot` 行为一致）；但**刚打开时可能还没出帧**，
> 这时返回 `{"ok":false,"error":"no frame"}`，隔一下重试即可。
>
> 阈值当前写死（置信度 0.25 / NMS IoU 0.45），没有可调参数。

### 响应

```json
{
  "ok": true,
  "count": 1,
  "boxes": [{"x1": 236.0, "y1": 88.5, "x2": 436.0, "y2": 283.5}]
}
```

| 字段 | 说明 |
|------|------|
| ok | 成功为 `true` |
| count | 框的个数；`0` 是**正常结果**（画面里没有目标） |
| boxes | 框列表，按分数降序、已完成类别内 NMS |

> **坐标是原图像素**（采集分辨率，默认 640x360），与 `GET /api/camera/snapshot` 返回的图
> 同一坐标系 —— 可以直接把框画到那张图上核对。
>
> 只回框的四个角，不回类别/分数/耗时。

### 失败

一律 `400` 或 `500` + `{"ok":false,"error":"..."}`：

| 情况 | HTTP | error 示例 |
|------|------|-----------|
| 没给 model | 400 | `缺少 model 参数（例：/api/detect?model=tennis）` |
| model 名字非法 | 400 | `model 名字非法（只允许字母数字与 _ - .）：../etc/passwd` |
| 模型不存在 / 加载失败 | 500 | `注册模型失败（CVI_NN_RegisterModel rc=…）：/root/AKA-00/demo/models/xxx.cvimodel` |
| 摄像头不可用 / 暂无帧 | 500 | `camera not available` / `no frame` |

### 示例

```bash
# 检测网球（模型 = $AKA_HOME/demo/models/tennis.cvimodel）
curl "http://<ip>/api/detect?model=tennis"

# 换另一颗模型
curl "http://<ip>/api/detect?model=block"
```

```json
{"boxes":[{"x1":234,"x2":434,"y1":88.5,"y2":285.5}],"count":1,"ok":true}
```

### 说明

- **模型只有一个来源**：部署目录下的 `demo/models/`（`make package` 整目录照搬）。裸名字只在
  库里查，不存在就报错，没有隐式回退。
- 接口是**同步**的：每个请求现场取帧 → 推理 → 返回。模型首次请求时加载，之后常驻；
  只有 `?model=` 变了才重新加载。
- **TPU 是单实例**：`/api/detect` 与流程脚本共用同一个检测器（各自串行），
  但别在脚本跑的时候另起一个吃 TPU 的进程。
- 换自己的模型时对一下规格。本仓库 `demo/models/tennis.cvimodel` 板上实测：输入
  `640x480`、`YUV420_PLANAR`、8 位量化；输出 `[1,5,6300,1]` FP32、单类别
  （`6300 = 80×60 + 40×30 + 20×15`，即三个 stride 的网格点数之和）。
  输入尺寸与格式都从模型张量里读，不写配置 —— 模型吃什么就喂什么。

---

## 模型管理

给外部调用方（平台）用：把模型送进部署目录的 `demo/models/` —— 也就是 `/api/detect` 唯一认的那个模型库。

### 上传模型（平台 → 小车，推荐）

```
POST /api/models/upload?name=<模型名>
Content-Type: application/octet-stream
（body = 模型文件的二进制内容）
```

```bash
# raw body：平台直接推文件（推荐）
curl --data-binary @tennis.cvimodel "http://<ip>/api/models/upload?name=tennis"

# multipart：浏览器 / form 客户端也行
curl -F "file=@tennis.cvimodel" "http://<ip>/api/models/upload?name=tennis"
```

| 参数 | 位置 | 必填 | 说明 |
|------|------|------|------|
| name | query | 是 | 模型名，落成 `$AKA_HOME/demo/models/<name>.cvimodel`。只允许字母数字与 `_ - .`，不允许 `/` 与 `..` |
| 文件 | body | 是 | 模型二进制（raw body，或 multipart 里名为 `file` 的字段） |

```json
{"ok": true, "name": "tennis", "path": "/root/AKA-00/demo/models/tennis.cvimodel", "size": 3540016}
```

同步接口：文件收完、校验通过、写盘换入之后才返回（3.5MB 的模型在内网上是一瞬间的事，不需要进度查询）。

> **为什么是"推"而不是"拉"**：小车在机器人的内网里（通常是热点/局域网），平台未必能被它反向访问；
> 这就是模型进入板子的**唯一**方式：由平台把文件推过来（不需要小车去访问平台，
> 也不需要板上有任何"模型商店/下载"的界面）。

**同名覆盖，且覆盖即生效**：`/api/detect` 每次请求都会 stat 模型文件，大小或 mtime 变了就重新加载
—— 换新版本不用重启 capp（代价是那一次请求多等一次模型加载）。

> 文件先落成 `.part`，校验通过后原子换入（`rename`）—— 传到一半、内容不对、中途断电都不会
> 破坏正在用的那颗模型。
>
> 校验两道：文件头必须是 `CviModel`（挡住"上传了别的文件"）；大小上限 **32MB**
> （请求体是整块读进内存的，板上可用内存约 50MB；模型实际约 3.5MB）。
>
> 校验只看文件头，所以「文件头对、内容是坏的」这种能被装上 —— 这时 `/api/detect` 会明确报
> `注册模型失败（CVI_NN_RegisterModel rc=…）`，重新传一个正确的即可，不需要别的清理动作。

| 失败 | HTTP | error 示例 |
|------|------|-----------|
| 没给 name | 400 | `name 参数必填（例：?name=tennis）` |
| name 非法 | 400 | `name 非法（只允许字母数字与 _ - .）：../etc/passwd` |
| 内容不是 cvimodel | 400 | `不是 cvimodel（文件头不是 CviModel）` |
| 请求体为空 | 400 | `请求体为空（把模型文件放进 body）` |
| 超过 32MB | 413 | `文件过大：34603008 字节，上限 32MB` |

### 训练平台直传（浏览器 → 小车）

训练平台（`yolotrain.chenlongrobot.com`）训练完，浏览器把模型**直传小车**（同一局域网），
车端不做任何运行切换，只落盘 —— 后续验证人工做。与上面那个接口的区别：名字在表单里
（走 query 的旧接口是给 curl / 云端推模型用的），响应字段是 `status/name/size`，
而且**会顺手给新槽位生成一份流程脚本**。

```
POST /api/model/upload
Content-Type: multipart/form-data

file = <模型二进制，文件名固定 model.cvimodel>    （必填）
name = <槽位名，如 orange>                        （必填）
```

```bash
curl -F "file=@model.cvimodel" -F "name=orange" "http://<ip>/api/model/upload"
```

成功：

```json
{"status":"ok","name":"orange","size":12865136,
 "path":"/root/AKA-00/demo/models/orange.cvimodel",
 "script":"/root/AKA-00/demo/orange.lua","script_created":true}
```

（`script` / `path` 是方便平台侧显示用的，不属于契约字段，忽略即可。）

| 失败 | HTTP | 响应 |
|------|------|------|
| file 为空 / 后缀不是 `.cvimodel` | 400 | `{"status":"error","message":"invalid file"}` |
| name 为空 / 含 `/`、`..` 等 | 400 | `{"status":"error","message":"invalid name"}` |
| 文件头不是 CviModel / 超过 32MB | 400 / 413 | `{"status":"error","message":"不是 cvimodel（文件头不是 CviModel）"}` |

落盘与副作用：

- 模型 → `demo/models/<name>.cvimodel`（**同名覆盖**，原子换入，坏包不会顶掉正在用的）
- 脚本 → `demo/<name>.lua`：拿 `demo/_template.lua` 把 `__MODEL__` 换成槽位名生成一份。
  **已存在则不动** —— 那份脚本可能已经手调过，重传模型不该把它冲掉。
  生成之后这个槽位在 Demo 页就能直接跑（模型 + 脚本 + 参数三样同名齐了）。
- CORS 与 `OPTIONS` 预检由服务器统一处理（所有响应带 `Access-Control-Allow-Origin: *`，
  预检回 200），浏览器跨域直传不需要额外配置。

## Demo（本地演示）

板上的 demo 就是"拿某个模型跑一遍抓取流程"。列表里有什么，取决于 `demo/models/` 里有什么
（demo 名 = 模型名），模型由平台推上来（见上）。

```
GET  /api/demo/list           → {"demos":[{"name":"tennis","kind":"model","script":"tennis"}, ...]}
POST /api/demo/init  {"name":"tennis"}   → 跑 demo/tennis.lua，参数取下面那份配置
POST /api/demo/stop                        → 停（等于 /api/script/stop）
```

### 运行参数（一个模型一份）

跑 demo 时传给脚本的参数，存在 `$AKA_HOME/demo/configs/<模型名>.json` ——
**一个模型一个文件，而且只在这张卡片上点过"保存"之后才存在**；没有文件就是内置默认值
（`target_size=300`、`speed=25`、`turn_speed=25`、`max_seconds=60`）。
界面上在 Demo 页每个 demo 卡片里编辑；接口是：

```
GET  /api/demo/config?name=tennis
     → {"name":"tennis","target_size":300,"speed":50,"turn_speed":25,"max_seconds":60}
POST /api/demo/config  {"name":"tennis","target_size":220,"speed":30,"turn_speed":30,"max_seconds":45}
```

| 字段 | 含义 |
|------|------|
| target_size | 目标框宽（原图像素）——框宽达到它就认为到位并抓取 |
| speed | 直线速度百分比（宿主还会再 clamp 到 ≤70） |
| turn_speed | 转弯速度百分比（同样 clamp 到 ≤70）—— 和直线分开：转弯要的占空比不同 |
| max_seconds | 单次运行的总时长上限（宿主强制，到点打断并停车） |

> 这些值就是脚本里 `params()` 读到的东西 —— 想给脚本加参数时，在这里加字段、
> 在脚本里读即可（见下一节）。
>
> **仓库是这份参数的唯一真源**：板上界面调好并保存的值，会在下一次 OTA 升级时被包里
> 带的那份覆盖。要正式改参数，就把值抄回仓库的 `demo/configs/<模型名>.json` 再部署
> （`demo/models/` 相反：平台运行时推上来的模型升级时会保留）。

---

## 流程脚本（Lua）

"看 → 对准 → 靠近 → 抓"这类**流程**天生要反复调参。写在 C++ 里，改一个数就得交叉编译 +
部署 + 重启（一轮几分钟）；写在脚本里就是改一行存盘重跑。所以 capp 内置了一个 Lua 宿主：
**原语在 C++（快、稳），流程在 `$AKA_HOME/demo/*.lua`（好改）**。

```
POST /api/script/run     {"script":"tennis", "max_seconds":30,
                          "params":{"model":"tennis","target_size":300,"speed":20}}
     → {"ok":true,"state":"running","script":"tennis","max_seconds":30}
GET  /api/script/status
     → {"state":"running","script":"tennis","message":"","calls":42,"action":"forward",
        "notes":{"box_w":"212","offset":"-33"}}
POST /api/script/stop
     → {"ok":true,"state":"aborted"}（立刻刹车，不等脚本配合）
```

| 字段 | 说明 |
|------|------|
| script | 脚本名，读 `$AKA_HOME/demo/<名字>.lua`。只允许字母数字与 `_ - .` |
| params | 传给脚本的参数（脚本用 `params()` 读），任意扁平/嵌套表 |
| max_seconds | **宿主强制**的总时长上限，默认 30，夹到 5~300 |

| state | 含义 |
|-------|------|
| `idle` | 没在跑 |
| `running` | 正在跑 |
| `done` | 脚本正常结束（`message` 是脚本的返回值） |
| `failed` | 失败：脚本 `fail()`、推理/相机出错、脚本语法错、超时、底盘掉线 |
| `aborted` | 被停止：`/api/script/stop`、人的运动指令接管、服务退出 |

### 脚本能用的原语（全部只有这些）

| 原语 | 说明 |
|------|------|
| `detect(model)` | 取一帧跑一次推理 → `{frame_w=640, boxes={{x1,y1,x2,y2,w,h,cx,cy,area},...}}`；硬失败返回 `nil, err`（"这一拍还没出帧"返回空列表，不是错误） |
| `forward(s)` `back(s)` `turn_left(s)` `turn_right(s)` `drive(l,r)` | 驱动；`s`/`l,r` 是百分比，**宿主一律 clamp 到 ±70** |
| `standby()` `brake()` | 速度归零 / 刹车 |
| `sleep_ms(ms)` | 等待（切段睡，随时可被打断） |
| `grab()` `release()` | 夹爪（ZP10S 下是"伸下去→夹→抬起"约 3.5s 的整段序列） |
| `elapsed_ms()` | 本脚本已跑的毫秒数 |
| `motor_connected()` | 底盘是否真在线（掉线时驱动是空操作，脚本可据此提前收手） |
| `abort_requested()` | 是否收到 stop（脚本可选择优雅收尾） |
| `note(k, v)` | 往 `/api/script/status` 的 `notes` 里发布一个可观测字段（调参用） |
| `log(fmt, ...)` | 写日志（`print` 也是它） |
| `fail(msg)` | 脚本主动判定失败 |
| `params()` | 启动时传进来的参数表 |

数学/字符串/table 标准库可用；**没有** io / os / package / coroutine / debug，也**没有 pcall**
（见下）。

### 安全边界（宿主强制，脚本绕不过去）

这是会真开电机的功能，所以下面这些都不在脚本手里：

| 约束 | 由谁强制 |
|------|---------|
| 速度上限 ±70% | 宿主 clamp 每个驱动原语的参数 |
| 总时长 | `max_seconds` + **看门狗**（每 2000 条 Lua 指令查一次，`while true do end` 也掐得住） |
| 被人的指令取代 | 脚本一驱动，宿主就记下指令代际号；摇杆/`/api/control` 一进来代际号就变，脚本立刻被中断并交出控制权 |
| stop / 服务退出 / 底盘掉线 | 同上，立刻中断 |
| 内存 | Lua VM 用带预算的分配器（4MB），脚本狂建 table 也吃不光板子内存 |
| 脚本吞掉中断 | **不给 pcall/xpcall** —— 脚本没法把宿主的打断 catch 住 |
| 退出时电机 | 宿主兜底刹车（脚本自己忘了停也一样） |

### 示例：`demo/tennis.lua`（追到目标并抓起来）

```bash
curl -X POST http://<ip>/api/camera/open
curl "http://<ip>/api/detect?model=tennis"      # 先看框多大，据此定 target_size
curl -X POST -H 'Content-Type: application/json' \
  -d '{"script":"tennis","max_seconds":30,"params":{"target_size":300,"speed":20}}' \
  http://<ip>/api/script/run
curl http://<ip>/api/script/status              # 边跑边看 action/notes
curl -X POST http://<ip>/api/script/stop        # 随时打断
```

判据与参数照搬隔壁仓库 `aka0/tennis.cpp`(那个预编译 demo 的源码，实机调过参)：取面积最大的框当
目标 → 偏出画面中心 ±80px 就先原地转（脉冲时长与偏离成正比，25~200ms 之间）→ 对准但框还不够大
就前进 150ms → 框宽达到 `target_size` 且居中就停稳、闭合夹爪。丢目标 1.5s 内没找回就收工。

与那套 demo 的三处**有意差异**：① 用框宽像素判定（本项目口径）而不是框面积占比；② 不做
"抓前左转 3 次"的爪子偏置补偿（实测夹空再加）；③ 丢目标即收工，不做没有超时的原地找球。

> 夹爪（ZP10S）**没有位置反馈**，"夹到没有"无法确认 —— 脚本只能报告"抓取序列已执行完"。
> 另外 **TPU 是单实例**：跑脚本时别同时跑 `demo/*/init.sh`（宿主会直接拒绝启动）。

---

## WiFi

### 扫描网络

```
GET /scan
```

### 连接

```
POST /connect
Content-Type: application/json

{"ssid": "WiFi名", "password": "密码"}
```

无密码时 `password` 为空字符串。

### 状态

```
GET /status
GET /api/ip
```

---

## OTA 固件升级

### 当前版本

```
GET /api/ota/version
```

### 检查更新

```
GET /api/ota/check
```

### 在线升级

```
POST /api/ota/upgrade
```

### OTA 状态

```
GET /api/ota/status
```

---

## 系统信息

```
GET /api/system/info
```

```json
{"ip": "192.168.4.1", "mac": "b8:27:eb:xx:xx:xx"}
```

```
GET /api/system/heartbeat
```

返回 CPU、内存、磁盘、运行时间等信息。