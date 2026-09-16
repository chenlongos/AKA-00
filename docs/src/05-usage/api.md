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
| model | string | 是 | 模型名，对应 `$AKA_HOME/models/<模型名>.cvimodel`（如 `tennis`、`block`）。只允许字母数字与 `_ - .`，不允许 `/` 与 `..` |

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
| 模型不存在 / 加载失败 | 500 | `注册模型失败（CVI_NN_RegisterModel rc=…）：/root/AKA-00/models/xxx.cvimodel` |
| 摄像头不可用 / 暂无帧 | 500 | `camera not available` / `no frame` |

### 示例

```bash
# 检测网球（模型 = $AKA_HOME/models/tennis.cvimodel）
curl "http://<ip>/api/detect?model=tennis"

# 换另一颗模型
curl "http://<ip>/api/detect?model=block"
```

```json
{"boxes":[{"x1":234,"x2":434,"y1":88.5,"y2":285.5}],"count":1,"ok":true}
```

### 说明

- **模型只有一个来源**：部署目录下的 `models/`（`make package` 整目录照搬）。裸名字只在
  库里查，不存在就报错，没有隐式回退。
- 接口是**同步**的：每个请求现场取帧 → 推理 → 返回。模型首次请求时加载，之后常驻；
  只有 `?model=` 变了才重新加载。
- **TPU 是单实例**：不要和 `demo/<名字>/init.sh` 同时跑（它 `exec` 的 `tennis` 二进制
  同样吃 TPU），会互相抢。
- 换自己的模型时对一下规格。本仓库 `models/tennis.cvimodel` 板上实测：输入
  `640x480`、`YUV420_PLANAR`、8 位量化；输出 `[1,5,6300,1]` FP32、单类别
  （`6300 = 80×60 + 40×30 + 20×15`，即三个 stride 的网格点数之和）。
  输入尺寸与格式都从模型张量里读，不写配置 —— 模型吃什么就喂什么。

---

## 模型管理

给外部调用方（平台）用：把模型送进部署目录的 `models/` —— 也就是 `/api/detect` 唯一认的那个模型库。

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
| name | query | 是 | 模型名，落成 `$AKA_HOME/models/<name>.cvimodel`。只允许字母数字与 `_ - .`，不允许 `/` 与 `..` |
| 文件 | body | 是 | 模型二进制（raw body，或 multipart 里名为 `file` 的字段） |

```json
{"ok": true, "name": "tennis", "path": "/root/AKA-00/models/tennis.cvimodel", "size": 3540016}
```

同步接口：文件收完、校验通过、写盘换入之后才返回（3.5MB 的模型在内网上是一瞬间的事，不需要进度查询）。

> **为什么是"推"而不是"拉"**：小车在机器人的内网里（通常是热点/局域网），平台未必能被它反向访问；
> 平台把文件直接推过来最省事。若你的场景恰好相反（小车能访问平台、平台进不来），用下面的拉取接口。

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