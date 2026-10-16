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
> **所有"会动"的请求都只回一个 `completed`**（true = 这次动作执行完了，失败时多一个
> `error` 说明原因；细节看 `/api/motor/status` 或日志）。三个入口一致：
> `?distance=`/`?angle=`、`?time=`、以及 `POST /api/demo/init|run` 带 `"wait": true`。
>
> 返回时机：
>
> | 请求 | 何时返回 | 返回 |
> |---|---|---|
> | `?distance=` / `?angle=` | **阻塞**到 ESP32 固件闭环报结果（最多 30s） | `{"completed": …}` |
> | `?...&time=` | **阻塞**到动作做完并自动停车 | `{"completed": …}` |
> | `?action=up`（不给 distance/time） | **立刻返回**（持续运动，靠 `?action=stop` 停） | `{status: success}` |
> | `?action=grab` / `release` | **阻塞**到夹爪那套序列做完（ZP10S 约 3.5s） | `{"completed": …}` |
>
> `grab`/`release` **不排队**：上一段还没做完时，后来的请求直接回
> `{"completed":false,"error":"夹爪正忙：上一段动作还没做完（这次没做，也没排队）"}`（400），
> 不会攒成一串挨个执行（实测连点 5 次 → 只执行 1 次）。
>
> `distance`/`angle` 的结论来自固件（它自己闭环 + 回状态），不是主机猜的 ——
> 车被卡住会回 `aborted`/`timeout` 而不是 `completed`。
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
| conf | float | 否 | 置信度阈值，默认 **0.25**。给的值会夹到 0.01~0.99 |
| iou | float | 否 | NMS 的 IoU 阈值，默认 **0.45**。管「两个框算不算同一个目标」（去重），不是置信度 |

> 摄像头没开会自动打开（与 `/api/camera/snapshot` 行为一致）；但**刚打开时可能还没出帧**，
> 这时返回 `{"ok":false,"error":"no frame"}`，隔一下重试即可。
>
> `conf` 管「这个框够不够可信」：调低减少漏检、调高压掉误检。
> `iou` 管「两个框要不要合成一个」：同一个目标画出两个框就调低它，
> 挨着的两个目标被吃掉一个就调高它。
>
> 两个参数给错值（不是数、或不在 0~1）返回 **400**，不会悄悄用默认值 —— 调参时最怕
> 「以为生效了其实没生效」。不给就是 0.25 / 0.45，与老版本行为完全一致。

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
- **TPU 是单实例**：`/api/detect` 与内置动作共用同一个检测器（各自串行），
  但别在动作跑的时候另起一个吃 TPU 的进程。
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

### 删除模型

```
POST /api/models/delete
{"name": "tennis"}
```

```json
{"ok": true, "name": "tennis", "cards": ["追网球接近"]}
```

| 失败 | HTTP | error 示例 |
|------|------|-----------|
| 没给 name | 400 | `name 必填（要删的模型名，不带 .cvimodel）` |
| name 非法 | 400 | `模型名非法（只允许字母数字与 _ - .）：../tennis` |
| 没有这个模型 | 400 | `没有这个模型：demo/models/tennis.cvimodel` |

**只删 `demo/models/<名字>.cvimodel` 这一个文件**（Demo 页上模型标签右上角那个 ✕ 走的就是它）。
用它建过的卡**不会跟着删**：卡片配置不动，只是变成 `ready=false`、点开始报 `模型文件缺失` ——
重传一个同名模型就原地复活。响应的 `cards` 是"正在用它的卡片名"，界面拿它提示后果。

> 板上删掉**包里自带**的模型只到下次 OTA 为止：升级按文件名取并集，同名会用包里的版本
> （见 `cpp/scripts/build-ota.sh`）。想让它彻底不来，得从 `cpp/board/demo/models/` 里去掉。

### 训练平台直传（浏览器 → 小车）

训练平台（`yolotrain.chenlongrobot.com`）训练完，浏览器把模型**直传小车**（同一局域网），
车端不做任何运行切换，只落盘 —— 后续验证人工做。与上面那个接口的区别：名字在表单里
（走 query 的旧接口是给 curl / 云端推模型用的），响应字段是 `status/name/size`。
模型落盘即可用：动作是内置的、与模型无关，**不给每个模型生成动作** ——
传完要么在 Demo 页新建一张卡片（动作 × 这个模型），要么直接
`POST /api/demo/init {"action":"grab","model":"orange"}`。

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
 "script":"","script_created":false,"actions":["approach","grab"]}
```

（`script` / `script_created` 是为兼容训练平台那份契约保留的字段，现在恒为 `""` / `false`；
`actions` 是当前可用的动作清单，方便平台侧提示"能用哪些动作"。都不属于必须消费的字段。）

| 失败 | HTTP | 响应 |
|------|------|------|
| file 为空 / 后缀不是 `.cvimodel` | 400 | `{"status":"error","message":"invalid file"}` |
| name 为空 / 含 `/`、`..` 等 | 400 | `{"status":"error","message":"invalid name"}` |
| 文件头不是 CviModel / 超过 32MB | 400 / 413 | `{"status":"error","message":"不是 cvimodel（文件头不是 CviModel）"}` |

落盘与副作用：

- 模型 → `demo/models/<name>.cvimodel`（**同名覆盖**，原子换入，坏包不会顶掉正在用的）
- 动作：**不再生成**。动作是内置在 aka-capp 里的 C++ 状态机（`grab` 追到就夹 / `approach` 只接近不夹），
  与模型无关；传完模型后要么在 Demo 页新建一张卡片（动作 × 这个模型），要么直接
  `POST /api/demo/init {"action":"grab","model":"<名字>"}` 跑一下。
- CORS 与 `OPTIONS` 预检由服务器统一处理（所有响应带 `Access-Control-Allow-Origin: *`，
  预检回 200），浏览器跨域直传不需要额外配置。

## Demo（本地演示）

板上的 **一张 demo 卡片 = 动作 × 模型**：

- **动作**是内置的 C++ 状态机（`grab` 追到就夹、`approach` 只接近不夹，见 `capp/script.cpp`，
  与模型无关；加新动作需改代码重编）；
- **模型**是 `demo/models/` 里的一颗 `.cvimodel`；
- 卡片由**用户在 Demo 页新建**（选动作、选模型、起个名字、填参数），名字随便起（中文也行），
  配置存在 `demo/configs/<卡片名>.json`。

```
GET  /api/demo/list                     → {"demos":[...], "actions":[...], "models":[...]}
POST /api/demo/init {"name":"追网球接近"}              → 跑存下来的那张卡片
POST /api/demo/init {"action":"grab","model":"tennis"} → 直接跑，不用建卡
POST /api/demo/init {"name":"追网球接近"}              → **默认等它跑完再返回**
POST /api/demo/stop                     → 停
```

`GET /api/demo/list` 一次给全三份数据（列表 + 可用的动作 + 可用的模型，新建表单直接用）：

```json
{
  "demos": [
    {"name":"追网球接近", "action":"approach", "model":"tennis",
     "ready":true, "script":"approach", "path":"/root/AKA-00/demo/models/tennis.cvimodel",
     "kind":"card", "error":""}
  ],
  "actions": [{"id":"approach","name":"接近瞄准"}, {"id":"grab","name":"追到就夹"}],
  "models": ["block", "tennis"]
}
```

> 动作的显示名来自内置注册表（`approach`="接近瞄准"、`grab`="追到就夹"）。
> `ready=false` 表示内置动作缺失或模型文件缺了（卡片照样列出来，点开始会明确报错）。

### 卡片配置（一张卡片一份）

```
GET  /api/demo/config?name=追网球接近
     → {"name":"追网球接近","action":"approach","model":"tennis",
        "target_size":300,"speed":50,"turn_speed":25,"mode":"once"}
POST /api/demo/config  {"name":"追网球接近","action":"approach","model":"tennis",
                        "target_size":300,"speed":30,"turn_speed":25,"mode":"loop"}
POST /api/demo/delete  {"name":"追网球接近"}
```

| 字段 | 含义 |
|------|------|
| action | 内置动作 id（`grab` / `approach`），必填 |
| model | 模型名（`demo/models/<model>.cvimodel`），必填 |
| target_size | 目标框宽（原图像素）——框宽达到它就认为到位 |
| speed | 直线速度百分比（宿主还会再 clamp 到 ≤70） |
| turn_speed | 转弯速度百分比（同样 clamp 到 ≤70）—— 和直线分开：转弯要的占空比不同 |
| mode | 执行方式：`once`（默认，跑一遍就结束，**最多 5 分钟**）/ `loop`（跑完接着跑，**直到你按停止**，没有时长上限） |

> **POST 就是"新建或覆盖一张卡片"**：界面上的"新建"与"保存"走的是同一个接口
> （改参数时要把 `action`/`model` 一起回传，否则会当成新建）。改名 = 用新名字 POST 一份、
> 把旧的 `POST /api/demo/delete` 掉。
>
> 这些值就是内置状态机读到的运行参数（另外宿主还会注入 `model`，见下节）。
>
> 卡片是**用户在板上建的现场数据**：OTA 升级时按"**板上优先**"保留 —— 同名卡片升级不会
> 覆盖你在界面上调好的参数（包里带的那些只在板上没有同名时才落地，当出厂预设）。
> 动作已内置进 aka-capp 二进制（不再用 `demo/*.lua`），随二进制一起升级 —— 想调参就改卡片配置。

### 临时组装一个 demo 直接跑（不建卡）

**"模型 + 动作 + 那几个值"凑齐就是一次完整的 demo 请求**，不用先建卡。刚传上来一个新
模型想立刻试、或者要把一条命令发给别人让他在板上按自己的参数跑一遍，都用这条：

```bash
curl -X POST http://<ip>/api/demo/init \
     -H 'Content-Type: application/json' \
     -d '{"action":"approach","model":"apple","target_size":320,"speed":30,"turn_speed":20,"mode":"once"}'
```

| 字段 | 含义 |
|------|------|
| action | 内置动作 id（`grab` / `approach`）—— 做什么 |
| model | 模型名（`demo/models/<model>.cvimodel`）—— 认什么 |
| target_size / speed / turn_speed | 与卡片里同名，缺省 300 / 25 / 25 |
| mode | `once`（默认，跑一遍）/ `loop`（跑完接着跑，直到 `POST /api/demo/stop`） |

效果与建一张卡再跑一样（宿主会把 `model=apple` 注入给动作）；区别是不落盘、
不会在 Demo 页留下卡片。想让它出现在页面上反复用，再按上面的卡片配置建成卡片。

### 跑完再返回（**默认行为**）

`POST /api/demo/init` / `/api/demo/run` **默认等这次跑完才返回**，直接给完成标志：

```bash
curl -X POST -H 'Content-Type: application/json' \
  -d '{"name":"追网球接近"}' http://<ip>/api/demo/init
# → {"completed": true}
# → {"completed": false, "error": "目标丢失（1520ms 没看到目标）"}
```

`completed: false` 时 `error` 说明原因（动作判定失败 / 丢目标 / 被人的指令接管 /
`timeout: 到最大执行时间（5 分钟）`）。过程中发生了什么看 `/api/demo/status` 的
`state` / `message` / `round` / `notes`。

> **执行一次（`mode: "once"`）有 5 分钟上限**，到点宿主自己收工（停电机、状态落 `aborted`、
> `error` 写"到最大执行时间"）—— 所以 `wait` 的请求最多 5 分钟必定有结论。
> 循环执行不受它管：`loop` 本来就不该自己结束，等不到就去 `POST /api/demo/stop`。
>
> 内置动作每一轮循环都会驱动电机/取帧（宿主在每次驱动动作入口查打断），不像旧 Lua 脚本
> 可能卡在不调用任何原语的死循环里——所以 `wait` 请求基本不会越过 5 分钟上限。

**想立刻返回**（不等，自己轮询状态 —— 界面就是这么用的）就显式传 `"wait": false`，
此时响应是 `{"status":"started", "name":…, "script":…}`：

```bash
curl -X POST -H 'Content-Type: application/json' \
  -d '{"name":"追网球接近","wait":false}' http://<ip>/api/demo/init
curl http://<ip>/api/demo/status          # 边跑边看
```

> **`mode=loop`（循环执行）例外**：它不会自己结束，所以默认**不等**（立刻回 `started`，
> 否则等于把连接挂死）；对它显式传 `"wait": true` 会被 **400** 拒掉
> （`loop 模式不会自己结束，wait 没有意义（要停就 POST /api/demo/stop）`）。要停就
> `POST /api/demo/stop`。

| 失败 | HTTP | 响应 |
|------|------|------|
| 卡片不存在 / 配置读不了 | 400 | `没有这张卡片（或配置读不了）：demo/configs/xxx.json` |
| 内置动作不存在 | 400 | `内置动作不存在：approach` |
| 模型不存在 | 400 | `模型不存在：demo/models/apple.cvimodel` |
| 名字非法（卡片名/动作名/模型名） | 400 | 各自说明原因（卡片名不能含 `/` `\\` 与控制字符） |

---

## 内置动作（C++ 状态机）

"看 → 对准 → 靠近 → 抓"这类**流程**原先写成 Lua 脚本（改一行存盘重跑、不用重编），现已
迁进 C++：动作**硬编码**成内置状态机（`grab` / `approach`，实现在 `capp/script.cpp`），不再读
`demo/*.lua`。最常改的运行参数（`model`/`target_size`/`speed`/`turn_speed`/`mode`）本来就来自卡片
配置、改它们不用重编；一批评据常量（转向脉冲系数、夹爪偏移等）也已**外置成卡片 JSON 的可选字段**
（不写则用出厂默认）。**安全兜底（限速/被人接管/底盘掉线/退出刹车）全在宿主里强制。**

```
POST /api/demo/run       {"script":"grab",
                          "params":{"model":"tennis","target_size":300,"speed":20}}
     → {"ok":true,"state":"running","script":"grab","mode":"once"}
GET  /api/demo/status
     → {"state":"running","script":"grab","model":"tennis","card":"追网球","message":"","calls":42,"action":"forward",
        "notes":{"box_w":"212","offset":"-33"}}
POST /api/demo/stop
     → {"ok":true,"state":"aborted"}（立刻刹车，不等状态机配合）
```

| 字段 | 说明 |
|------|------|
| script | **动作名**，内置动作 id（`grab` / `approach`）。只允许字母数字与 `_ - .` |
| params | 运行参数 + 可选的调参常量覆盖（见下） |
| params.mode | `once`（默认）跑一遍就结束，**最多 5 分钟**（到点宿主收工）/ `loop` 跑完接着跑直到被停，没有时长上限 |

| state | 含义 |
|-------|------|
| `idle` | 没在跑 |
| `running` | 正在跑 |
| `done` | 正常结束（`message` 是本次运行的结论，如"已抓取"/"已到位"/"目标丢失"） |
| `failed` | 失败：推理/相机出错、模型名非法、必填参数缺失、底盘掉线 |
| `aborted` | 被停止：`/api/demo/stop`、人的运动指令接管、服务退出、`once` 到最大执行时间 |

### 运行参数（必填/常用）

| 参数 | 默认 | 说明 |
|------|------|------|
| model | 必填 | 用哪个模型找目标（`demo/models/<model>.cvimodel`） |
| target_size | 必填(>0) | 目标框宽（原图像素）；框宽达到它就认为到位 |
| speed | 20 | 直线速度百分比（宿主仍 clamp 到 ≤70） |
| turn_speed | =speed | 转弯速度百分比 |
| mode | once | `once` / `loop` |

### 可调的判据常量（卡片 JSON 可选字段，不写则用出厂默认）

这些是实测调过、改动很低的低频常量。把它们**写进卡片配置 `demo/configs/<卡片名>.json`**
（数值型，与运行参数同级）即可覆盖出厂默认；界面暂不暴露这些字段，需直接编辑 JSON 文件。

| 键 | 出厂默认 | 含义 |
|------|---------|------|
| `center_margin` | 80 | 偏出这个像素范围就来一次大转向 |
| `turn_pulse_k` | 0.5 | 转向脉冲系数（ms/px，偏离越多转越久） |
| `turn_pulse_min` / `turn_pulse_max` | 300 / 400 | 转向脉冲上下限（下限必须 > 电机启动时间 ~250ms） |
| `fwd_pulse_ms` | 600 | 前进脉冲时长（必须 > 电机启动时间） |
| `align_margin` | 25 | 到位判据：对到 `grab_offset` 附近这么小才敢夹 |
| `too_close_k` | 1.5 | 框宽超过 `target_size` 这个倍数 = 凑得太近 |
| `back_pulse_k` | 2.0 | 后退脉冲系数（ms/px，超出的像素数） |
| `back_pulse_min` / `back_pulse_max` | 300 / 700 | 后退脉冲上下限 |
| `grab_offset` | 50 | 夹爪相对画面中心的正偏移（只能实测调） |
| `fine_pulse_max` | 500 | 精调转向的脉冲上限（够大但没对准时用，别转过头） |
| `lost_ms` | 1500 | 连续多久看不到目标就收工 |
| `loop_gap_ms` | 30 | 每步之间喘口气，让相机出新帧 |
| `conf` / `iou` | 0（用 0.25 / 0.45） | detect 解码阈值 |

> **注意**：从界面重新保存卡片（`POST /api/demo/config`）只会回写上面"运行参数"那几项，
> 会把手工写进 JSON 的判据常量重置为默认。手工精调 = 直接编辑 `demo/configs/<卡片名>.json`。

### 安全边界（宿主强制）

这是会真开电机的功能，所以下面这些都不由状态机决定：

| 约束 | 由谁强制 |
|------|---------|
| 速度上限 ±70% | 宿主 clamp 每个驱动动作的参数 |
| 执行方式 | `mode`：`once` 跑一遍，**最多 5 分钟**（到点宿主收工）；`loop` 循环跑，没有总时长上限 —— 停不停由你按停止决定 |
| 被人的指令取代 | 一驱动，宿主就记下指令代际号；摇杆/`/api/control` 一进来代际号就变，运行立刻被中断并交出控制权 |
| stop / 服务退出 / 底盘掉线 | 同上，立刻中断 |
| 退出时电机 | 宿主兜底刹车（状态机自己忘了停也一样） |

### 示例：`grab`（追到目标并抓起来）/ `approach`（只接近不夹）

```bash
curl -X POST http://<ip>/api/camera/open
curl "http://<ip>/api/detect?model=tennis"      # 先看框多大，据此定 target_size
curl -X POST -H 'Content-Type: application/json' \
  -d '{"script":"grab","params":{"model":"tennis","target_size":300,"speed":20,"mode":"once"}}' \
  http://<ip>/api/demo/run
curl http://<ip>/api/demo/status                # 边跑边看 action/notes
curl -X POST http://<ip>/api/demo/stop          # 随时打断
```

判据（"框宽 = 距离"这一条轴）：取面积最大的框当目标，然后五选一 ——

| 情况 | 动作 |
|------|------|
| 框宽 > 目标 × `too_close_k`（**凑太近**） | 后退一小段：脉冲 = `back_pulse_k` × 超出量，夹在 `back_pulse_min`~`max` |
| 框宽 ≥ 目标 且 与夹爪位差 ≤ `align_margin` | **到位**：停稳 → 闭合夹爪（`approach` 则只停不夹） |
| 偏离画面中心 > `center_margin` | 大脉冲转向：脉冲 = `turn_pulse_k` × 偏离，夹在 `turn_pulse_min`~`max` |
| 框宽 ≥ 目标 但没对准夹爪位 | 精调小脉冲转向（同上，上限 `fine_pulse_max`，别转过头） |
| 框宽 < 目标 | 前进 `fwd_pulse_ms` |

丢目标 `lost_ms` 没找回就收工。三处脉冲的上下限都不是拍的：下限必须大于电机启动时间
（板上实测这块底盘 ~250ms 才转得起来），上限是"别转过头/退过头"（车尾没有眼睛）。

两处**有意设计**：① 用框宽像素判定（本项目口径）而不是框面积占比；② **凑太近会先退一小段**
（车几乎贴上去时夹爪反而够不着）。夹爪偏置补偿不做（实测夹空再调 `grab_offset`）。

> 夹爪（ZP10S）**没有位置反馈**，"夹到没有"无法确认 —— 只能报告"抓取序列已执行完"。
> 另外 **TPU 是单实例**：跑动作时别再并发调用 `/api/detect`（宿主内部串行，但会互相拖慢）。

---

## WiFi

都作用在 **`wlan1`** 上（`wlan0` 是那台给用户连的 AP 热点，不动它）。

### 扫描网络

```
GET /api/wifi/scan
```

```json
{"list": [{"ssid": "…", "id": "…", "signal": -52, "secured": true, "is_connected": false}],
 "connected": "…"}
```

阻塞最多约 5 秒；排序是「已连接优先，其次信号从强到弱」，同名网络只留最强的那个。

### 连接

```
POST /api/wifi/connect
Content-Type: application/json

{"ssid": "WiFi名", "password": "密码"}
```

无密码时 `password` 传空字符串。阻塞约 8 秒等关联、再加最多 6 秒等 DHCP，成功回
`{"ip": "…"}`（还没拿到地址时回 `"获取中..."`）；失败回 **408** +
`{"error":"连接超时" | "连接失败，请检查密码或信号" | "未找到该网络"}`。

连接成功会把 `{ssid, password}` 记进 `/etc/aka-wifi.json`（`0600`），capp 每次启动在
后台重放一次，所以**不用每次开机都重连**（只保留最后一个；`rm` 掉即"忘记网络"）。

### 状态

```
GET /api/wifi/status     # {"ssid": "…"|null, "ip": "…"}
GET /api/wifi/ip         # {"ip": "…"}（没连上时回热点地址 192.168.4.1）
```

### 系统 IP

```
GET /api/system/ip       # {"ip": "…"}
```

---

## OTA 固件升级

### 当前版本

```
GET /api/ota/version       # {"version": "v0.6.1", "updated": 1730000000, "service": "AKA-00"}
```

### 检查更新

```
GET /api/ota/check
```

去 `config.toml` 的 `[ota] check_url` 取版本信息，比语义版本号（退而比 `updatedAt`）：

```json
{"current_version": "…", "update_available": true, "latest_version": "…",
 "hardware_desc": "…", "software_desc": "…", "url": "…"}
```

没有可用更新时回 **404** `{"status":"error","message":"未找到可用更新"}`。

### 在线升级

```
POST /api/ota/upgrade
```

异步下载固件并安装，**立刻**回 `{"status":"ok","task_id":"…"}`；已经是新版则回
`{"status":"ok","message":"已是最新版本"}`。固件与暂存都在 `$AKA_HOME/.ota`（不放 `/tmp`，
那是内存盘）。

### 直接上传固件升级（不联网）

```
POST /api/ota/update       # multipart/form-data，文件部分就是固件
```

回 `{"status":"ok","task_id":"…","message":"upload received, installing..."}`；没有文件回
**400** `{"status":"error","message":"no firmware file"}`。

### OTA 状态

```
GET /api/ota/status                              # 没有进行中的任务时 {"status":"idle"}
GET /api/ota/upgrade/progress?task_id=<id>       # {"progress": 0-100, "status": "downloading|installing|done|error", "message": "…"}
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