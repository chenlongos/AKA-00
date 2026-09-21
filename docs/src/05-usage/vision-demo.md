# 检测 · 识别 · Demo 运行 API

给调用方（平台、脚本、别人写的程序）的一份**独立**文档：从"看一眼画面"到"让小车自己追过去
把东西夹起来"这一整条链的 HTTP 接口。全部在板上实测过，**坐标单位一律是原图像素**。

> 想一次看全部接口（电机/机械臂/OTA/WiFi/系统）见 [API 文档](./api.md)；这篇只讲
> **摄像头 → 推理 → 跑动作**这条链，覆盖 `/api/camera/*`、`/api/detect`、`/api/models/*`
> 与 `/api/demo/*`。

## 一分钟上手

```bash
IP=<板子IP>

# ① 开摄像头（检测的前提；不开会回 "camera not available"）
curl -X POST http://$IP/api/camera/open

# ② 看一眼识别结果（框的四个角，原图像素）
curl "http://$IP/api/detect?model=tennis"
# → {"ok":true,"count":1,"boxes":[{"x1":2,"x2":172,"y1":0,"y2":360}]}

# ③ 跑一次 demo：用 tennis 模型做"接近瞄准"，框宽到 320px 就算到位
curl -X POST http://$IP/api/demo/init -H 'Content-Type: application/json' \
     -d '{"action":"approach","model":"tennis","target_size":320,"speed":30,"turn_speed":25}'
# → {"completed":true}          ← 默认等它跑完再返回（下面的"wait"一节）
```

③ 里**没有建任何卡片**：`action + model + 参数`凑齐就是一次完整请求。车会真的动。

---

## 1. 摄像头（检测的前提）

| 接口 | 说明 |
|------|------|
| `GET /api/camera/status` | `{"camera_on": true/false}` |
| `POST /api/camera/open` | 打开采集。成功 200 `{"camera_on":true}`；打不开则 **500 + `{"camera_on":false}`**（原因见板子日志） |
| `POST /api/camera/close` | 关闭采集（顺带停屏） |
| `GET /api/camera/snapshot` | 单帧 JPEG，**base64 塞在 JSON 里**（见下） |
| `GET /api/camera/stream` | MJPEG 流（`multipart/x-mixed-replace`），直接给 `<img src="http://<ip>/api/camera/stream">` |

`snapshot` 的响应（实测 640x360 q70 约 46KB JSON）：

```json
{"image":"/9j/4AAhQVZ...", "format":"jpeg", "width":640, "height":360, "m":0.05, "c":-2.82}
```

> `m` / `c` 是**像素→实际距离**的标定系数（`config.toml` 的 `[camera] calib_m/calib_c`），
> 用来自算"这个框离我多远"。不需要就算着玩，`detect` 不用它们。

**分辨率/帧率/画质都在 `config.toml` 的 `[camera]`**（`width/height/fps/jpeg_quality`），
接口里不能改。`stream` 默认直通摄像头原始 MJPEG（服务端零解码零编码），所以带宽约
4~7 Mbps；嫌大就调低 `jpeg_quality`。

---

## 2. 检测 / 识别：`GET /api/detect`

**"检测"和"识别"在这个系统里是同一件事**：把一帧送进模型，拿回几个框。模型认什么由它自己
训练决定（网球、方块、橘子…），接口不管语义 —— 它只把框给你。

```
GET /api/detect?model=<模型名>[&conf=0.25][&iou=0.45]
```

| 参数 | 位置 | 必填 | 说明 |
|------|------|------|------|
| model | query | 是 | 模型名（= `demo/models/<名字>.cvimodel` 的文件名，不带后缀）。只允许字母数字与 `_ - .` |
| conf | query | 否 | 置信度阈值，`(0,1)`；不给用 0.25 |
| iou | query | 否 | NMS 的 IoU 阈值，`(0,1)`；不给用 0.45 |

成功（HTTP 200）：

```json
{"ok": true, "count": 1, "boxes": [{"x1": 2, "x2": 172, "y1": 0, "y2": 360}]}
```

| 字段 | 含义 |
|------|------|
| boxes[].x1 / y1 | 左上角（原图像素，浮点） |
| boxes[].x2 / y2 | 右下角 |
| count | 框数（可能 0 个 —— 那不是错误） |

> **框宽 = `x2 - x1`，中心 = `(x1+x2)/2, (y1+y2)/2`** —— REST 版只给四角，宽高/中心/面积
> 自己算。脚本里的 `detect()` 会多给 `w/h/cx/cy/area`（见 §5）。
>
> 坐标是**原图**（如 640x360），**不是**页面上 `stream` 缩放后的尺寸 —— 两个尺寸不一致时
> 别拿标尺去量屏幕。

失败：

| 情况 | HTTP | 响应 |
|------|------|------|
| 没给 model | 400 | `{"ok":false,"error":"缺少 model 参数（例：/api/detect?model=tennis）"}` |
| model 名字非法 | 400 | `{"ok":false,"error":"model 名字非法（只允许字母数字与 _ - .）：../x"}` |
| conf/iou 越界或不是数 | 400 | `{"ok":false,"error":"conf / iou 要在 0~1 之间（如 ?conf=0.6&iou=0.3）；不给就用默认 0.25 / 0.45"}` |
| **摄像头不可用**（没开、或设备打不开） | 500 | `{"ok":false,"error":"camera not available"}` |
| 摄像头开着但**这一拍还没出帧** | 500 | `{"ok":false,"error":"no frame"}` |
| 模型文件不在或坏了 | 500 | `{"ok":false,"error":"注册模型失败（CVI_NN_RegisterModel rc=…）"}` 之类 |

> **要"连续看"就轮询这个接口**（车上一帧推理约 100~340ms，取决于有没有同时写着屏）；
> 要高帧率画面用 `/api/camera/stream`，它不跑模型。
>
> `conf` 给错值**直接报错**而不是悄悄用默认 —— 调参时最怕"以为生效了其实没生效"。

---

## 3. 模型（"认什么"）

模型就是 `$AKA_HOME/demo/models/<名字>.cvimodel` 一个文件，**文件名去掉后缀就是 `model` 参数的值**。
`GET /api/demo/list` 的 `models` 字段能拿到板上现有的清单：

```bash
curl http://$IP/api/demo/list
# → {"demos":[…], "actions":[{"id":"approach","name":"接近瞄准"},…], "models":["block","orange","tennis"]}
```

### 上传

| 接口 | 谁用 | 怎么发 |
|------|------|--------|
| `POST /api/models/upload?name=<名字>` | 平台 / curl **推文件** | 名字在 query，body 就是文件裸内容（也认 multipart 的 `file` 字段） |
| `POST /api/model/upload` | **浏览器表单直传**（CORS 已开） | multipart，两个字段 `file` + `name` |

```bash
curl --data-binary @orange.cvimodel "http://$IP/api/models/upload?name=orange"
# → {"ok":true,"name":"orange","path":"/root/AKA-00/demo/models/orange.cvimodel","size":12864632}

curl -F "file=@orange.cvimodel" -F "name=orange" "http://$IP/api/model/upload"
# → {"status":"ok","name":"orange","size":12864632,"path":"…"}
```

**同名覆盖，且覆盖即生效**（下一次 `/api/detect` 就用新模型，不用重启）。校验：文件头必须是
`CviModel`（挡"传错文件"）、大小上限 **32MB**；先落 `.part` 再原子换入，传一半断了不会毁掉
正在用的那颗。

### 删除

```bash
curl -X POST http://$IP/api/models/delete -H 'Content-Type: application/json' -d '{"name":"orange"}'
# → {"ok":true,"name":"orange","cards":["追橘子"]}      ← cards = 正在用它的卡片名
```

只删这一个文件。**用它建过的卡片不会跟着删**，只是变成 `ready:false`、点开始报
`模型文件缺失` —— 重传一个同名模型就原地复活。删完记得 `GET /api/demo/list` 刷新清单。

---

## 4. Demo 运行

### 概念：一张卡片 = 动作 × 模型

- **动作** = `demo/<动作>.lua` 脚本（预定义、与模型无关：`approach` 接近瞄准、`grab` 追到就夹）；
- **模型** = `demo/models/<模型>.cvimodel`；
- **卡片** = 用户起的名字 + 上面两者 + 四个参数，存 `demo/configs/<卡片名>.json`。

**卡片不是必需的**：`action + model + 参数` 直接发就是一次完整请求（不落盘、页面不留卡片）。
卡片只是"存下来反复用"的壳。

### 跑一次：`POST /api/demo/init`

两种形状，**二选一**：

```jsonc
// ① 跑存下来的卡片（界面上点"开始"就是这条）
{"name": "追网球接近"}

// ② 临时组装一次（不用建卡）
{"action": "approach", "model": "tennis",
 "target_size": 320, "speed": 30, "turn_speed": 25, "mode": "once"}
```

| 字段 | 说明 |
|------|------|
| name | 卡片名。**给了它就以卡片为准**，下面那些参数是"临时覆盖"，不落盘 |
| action | 动作名（`demo/<action>.lua`）。没有 name 时必填 |
| model | 模型名。没有 name 时必填 |
| target_size | 目标框宽（原图像素）：框宽达到它就认为到位。缺省 **300** |
| speed | 直线速度百分比。缺省 **25**（宿主还会再 clamp 到 ≤70） |
| turn_speed | 转弯速度百分比。缺省 **25** |
| mode | `once`（默认，跑一遍）/ `loop`（跑完接着跑，**直到你按停止**，没有时长上限） |
| wait | 见下节。**默认 true** |

响应：

```jsonc
// 默认（wait=true）：等这次跑完再返回，只给一个完成标志
{"completed": true}
{"completed": false, "error": "目标丢失（1595ms 没看到目标）"}     // 跑完了但没成功
{"completed": false, "error": "timeout: 到最大执行时间（5 分钟）"}   // 到时限被宿主收工

// wait=false：立刻返回"起来了"
{"status":"started","name":"approach","script":"approach","action":"approach",
 "model":"tennis","pid":682,"pgid":682,"completed":false}
```

**`wait` 的语义**（最容易被误解的一条）：

- 它只决定**这一个 HTTP 请求等不等**，不是脚本的时限。
- `once` 默认等 → 一次请求拿到结论；`loop` **默认不等**（循环不会自己结束，等下去就是挂死
  连接），显式传 `"wait": true` 直接 **400**。
- 请求最多等 **310 秒**；而**脚本自身**在 `once` 模式下最多跑 **5 分钟**（到点宿主收工、
  状态落 `aborted`、`error` 写明"到最大执行时间"）。所以正常情况是先由脚本收工、请求拿到
  真实原因。
- 想自己轮询就传 `"wait": false`，然后看 `/api/demo/status`。

失败响应：

| 情况 | HTTP | 响应 |
|------|------|------|
| 卡片不存在 | 400 | `{"error":"没有这张卡片（或配置读不了）：demo/configs/xxx.json"}` |
| 卡片名非法 | 400 | `{"error":"卡片名非法（不能含 / \\ 与控制字符，不能以 . 开头）：../x"}` |
| 动作脚本不存在 | 400 | `{"error":"动作脚本不存在：demo/<动作>.lua"}` |
| 模型文件不存在 | 400 | `{"error":"模型不存在：demo/models/<模型>.cvimodel"}` |
| 没给 name 也没给 action+model | 400 | `{"error":"要么给 name（跑已建的卡片），要么给 action + model（直接跑）"}` |
| loop + `"wait": true` | 400 | `{"error":"loop 模式不会自己结束，wait 没有意义（要停就 POST /api/demo/stop）"}` |
| **已经有一个脚本在跑** | 409 | `{"status":"already_running","pid":…,"name":"<在跑的卡片>","error":"已有脚本在跑（先 POST /api/demo/stop）"}` |

> **一次只能跑一个**：脚本驱动电机是独占的，第二个请求 409。要换就 `stop` 再起。

### 最底层那条：`POST /api/demo/run`

```json
{"script": "grab", "params": {"model": "tennis", "target_size": 300, "mode": "once"}}
```

和 `init` 的区别：**不校验模型/卡片**，`params` 原样交给脚本（调试、一次性用）。响应字段与
`init` 一致（`{ok,state,script,mode}` / 等跑完时 `{completed:…}`）。

### 看状态：`GET /api/demo/status`

```json
{"state":"running","script":"approach","model":"tennis","card":"追网球接近","mode":"once",
 "round":1,"calls":92,"action":"forward","message":"",
 "notes":{"box_w":"298","offset":"28"}}
```

| 字段 | 含义 |
|------|------|
| state | `idle`（没在跑）/ `running` / `done`（正常结束）/ `failed`（脚本 fail、推理/相机错、底盘掉线…）/ `aborted`（被停：stop、人的指令接管、服务退出、**或 once 到 5 分钟时限**） |
| script | 正在跑的动作名 |
| model / card | 在追哪个模型 / 哪张卡片（直接 action+model 跑时 `card` 为空串） |
| mode / round | 执行方式 / 循环跑到第几轮 |
| message | 结束原因或脚本的返回值（`done` 时就是脚本 `return` 的那句话） |
| action | 最近一次电机原语（`forward`/`turn_left`/`standby`…），空 = 还没动过 |
| calls | 脚本调用原语的次数（心跳/活性） |
| **notes** | 脚本用 `note(k,v)` 发布的自定义观测值（如 `box_w`/`offset`）—— **调参就靠它** |

### 停：`POST /api/demo/stop`

立刻刹车、不等脚本配合；脚本会在下一次调原语时被中断，状态转 `aborted`。
没在跑时返回 `{"status":"already_stopped"}`（不报错）。

### 卡片管理

| 接口 | 说明 |
|------|------|
| `GET /api/demo/list` | 卡片 + 动作 + 模型清单（新建卡片的下拉就用它）。卡片字段：`name/action/model/ready/error/kind/script/path` |
| `GET /api/demo/config?name=<卡片名>` | 读一张卡的参数（`{name,action,model,target_size,speed,turn_speed,mode}`） |
| `POST /api/demo/config` | 新建或覆盖：`{"name":…,"action":…,"model":…,"target_size":…,"speed":…,"turn_speed":…,"mode":…}`。**改参数时 action/model 也要回传**，否则会被当成新建 |
| `POST /api/demo/delete` | `{"name":"<卡片名>"}` —— 只删这张卡；动作脚本与模型文件不受影响 |
| `GET /api/demo/name` | `{name,action,model}`：正在跑的是哪张卡（老接口，等价于 status 里的 `card`） |

`ready:false` 表示动作脚本或模型文件缺了 —— 卡片照样列出来（点开始会明确报错），不会凭空消失。

---

## 5. 动作脚本（要加新动作时）

动作就是 `$AKA_HOME/demo/<动作名>.lua` 一个文件，宿主内置 Lua 解释器跑它。
**第一行的约定注释给显示名**（界面上的"接近瞄准"就是这么来的）：

```lua
-- name: 我的动作
local p = params() or {}
local model = p.model                      -- 宿主注入：卡片/请求里配的模型
local target = tonumber(p.target_size) or 300

while true do
    local d, err = detect(model)           -- 取一帧跑一次推理
    if not d then fail("推理失败：" .. tostring(err)) end
    local best = nil
    for _, b in ipairs(d.boxes) do
        if not best or b.area > best.area then best = b end
    end
    if not best then standby(); sleep_ms(50)
    elseif best.x2 - best.x1 >= target then brake(); return "到位了"
    else forward(p.speed or 20); sleep_ms(600); standby() end
end
```

### 能用的原语（全部只有这些）

| 原语 | 说明 |
|------|------|
| `detect(model, opts?)` | 取一帧推理；`opts` 可给 `{conf=, iou=}`（默认 0.25 / 0.45）→ `{frame_w=640, boxes={{x1,y1,x2,y2,w,h,cx,cy,area},…}}`；**硬失败**返回 `nil, err`（"这一拍还没出帧"给空列表，不算错误） |
| `forward(s)` `back(s)` `turn_left(s)` `turn_right(s)` `drive(l,r)` | 驱动，参数是百分比，**宿主一律 clamp 到 ±70** |
| `standby()` `brake()` | 速度归零 / 刹车 |
| `sleep_ms(ms)` | 等待（切段睡，随时可被打断） |
| `grab()` `release()` | 夹爪（ZP10S 上是"伸下去→夹→抬起"约 3.5s 的整段序列） |
| `elapsed_ms()` | 本脚本已跑的毫秒数 |
| `motor_connected()` | 底盘是否真在线（掉线时驱动是空操作） |
| `abort_requested()` | 是否收到 stop（可选优雅收尾） |
| `note(k, v)` | 往 `/api/demo/status` 的 `notes` 里发一个可观测值 |
| `log(fmt, …)` / `print` | 写进 capp 日志 |
| `fail(msg)` | 主动判定失败（状态 `failed`） |
| `params()` | 启动时传进来的参数表 |

有 math/string/table 标准库；**没有** io / os / package / coroutine / debug，也**没有 pcall**
（脚本吞掉宿主的打断就糟了）。内存预算 4MB。

> 板上现成两个动作：`approach`（`-- name: 接近瞄准`，只靠近不夹）、`grab`（`-- name: 追到就夹`）。
> **别改这两个文件** —— 它们是仓库里的代码、OTA 会按包覆盖；想调参请改卡片配置或另写一个
> 动作脚本（`demo/<新名字>.lua`，OTA 不会删你新加的）。

---

## 6. 安全边界（宿主强制，脚本和调用方都绕不过去）

| 约束 | 说明 |
|------|------|
| 速度上限 ±70% | 每个驱动原语的参数都被 clamp |
| 一次一个脚本 | 再发 `init`/`run` 会 409，先 `stop` |
| `once` 最多 5 分钟 | 到点宿主收工（停电机、状态 `aborted`、`error` 写明原因） |
| `loop` 没有时长上限 | 它本来就该一直跑，停不停由人决定 |
| 被人的指令取代 | 有人推摇杆 / 调 `/api/control`，脚本立刻被中断并交出控制权 |
| stop / 服务退出 / 底盘掉线 | 立刻中断（脚本收尾时宿主兜底刹车） |
| 内存 | Lua VM 4MB 预算 |

---

## 7. 一条龙例子（可直接复制）

```bash
IP=<板子IP>; MODEL=tennis

# 1) 传一颗模型（同名覆盖、覆盖即生效）
curl --data-binary @orange.cvimodel "http://$IP/api/models/upload?name=orange"

# 2) 开摄像头，看它在哪、框多大（据此定 target_size）
curl -X POST http://$IP/api/camera/open
curl "http://$IP/api/detect?model=$MODEL&conf=0.5"

# 3) 跑一次：不建卡，直接组装；默认等跑完
curl -X POST http://$IP/api/demo/init -H 'Content-Type: application/json' \
     -d "{\"action\":\"approach\",\"model\":\"$MODEL\",\"target_size\":320,\"speed\":30,\"turn_speed\":25}"

# 4) 想边跑边看：wait=false + 轮询 status（notes 里有 box_w/offset，调参就看它）
curl -X POST http://$IP/api/demo/init -H 'Content-Type: application/json' \
     -d "{\"action\":\"approach\",\"model\":\"$MODEL\",\"target_size\":320,\"wait\":false}"
while true; do curl -s http://$IP/api/demo/status; echo; sleep 1; done

# 5) 停
curl -X POST http://$IP/api/demo/stop
```

## 8. 常见坑（都是踩过的）

| 现象 | 原因 |
|------|------|
| `/api/detect` 回 `camera not available` | 摄像头没开（先 `POST /api/camera/open`），或设备打不开 |
| `/api/detect` 回 `no frame` | 摄像头开着、但这一拍还没出帧（刚开没多久）；连续这样就是流卡住了 |
| `POST /api/camera/open` 一直 500、但 `snapshot` 还能出图 | 采集流卡死、缓存里还有旧帧（两次 snapshot 字节完全相同就是在骗你）。**重启 capp 恢复**（实测 2026-09-21；根因待查：卡死后没法从 API 侧再拉起来） |
| 框的坐标对不上屏幕 | 坐标是**原图**像素；页面上的画面可能被缩放过 |
| `init` 一直不返回 | 默认 `wait=true`，它在等脚本跑完（最多 310 秒）。想立刻拿控制权就 `wait:false` |
| 请求回 `timeout: 等待 310 秒…` 但车还在动 | 请求放弃等了，脚本还在跑 —— 补一个 `POST /api/demo/stop` |
| `loop` 模式传了 `wait:true` 被 400 | 循环不会自己结束，等下去是挂死连接 |
| 第二个 `init` 回 409 | 已经有一个脚本在跑，先 `stop` |
| 删了模型，卡片还在但点开始报错 | 卡片不跟着删（故意的）；重传同名模型即复活 |
| 传模型回 `不是 cvimodel（文件头不是 CviModel）` | 传错文件了 |
| 相机开着但屏没画面（带屏板） | 屏跟随摄像头：摄像头一开就自动起屏；`/api/display/status` 看 `running` |
