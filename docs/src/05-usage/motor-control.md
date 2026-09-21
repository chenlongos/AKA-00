# 底盘 · 电机控制 API（前端对接用）

面向**写界面/客户端**的人：怎么让小车动、怎么读它的实时状态、怎么让摇杆跟手。
覆盖 `/api/control`、`/api/motor/*`、`/api/config/speed`，以及 `/ws/control` 里与驱动有关的部分。
下面所有响应都是板上实测的。

> 本文只管"底盘怎么动"。推理与 demo 见 [检测 · 识别 · Demo 运行 API](./vision-demo.md)，
> WebSocket 完整协议见 [WebSocket 控制接口](./websocket.md)。

## 0. 四条路，先选对

| 你要做的事 | 用哪个 |
|---|---|
| 摇杆 / 连续控制（**推荐**） | WebSocket `/ws/control` 的 `{"type":"action",…}`，顺带收轮速推送 |
| "往前走一段 / 转 90 度" | `GET /api/control?action=…&time=/distance=/angle=` |
| 精确控制左右轮（差速、自己算闭环） | `GET /api/motor/direct?left=&right=&duration=` |
| 显示速度 / 连接状态 / 夹爪 | `GET /api/motor/status`（轮询）或 WS 推送 |
| 界面上的"默认速度" | `GET/POST /api/config/speed`（后端只存不读，见 §4） |

---

## 1. `GET /api/control` —— 命名动作

```
GET /api/control?action=up&speed=40&time=500
```

| 参数 | 单位 | 说明 |
|------|------|------|
| action | — | `up`（前进）/ `down`（后退）/ `left` / `right` / `stop`；另有臂动作 `grab` / `release` |
| speed | % | **1~100**（<1 抬到 1，>100 压到 100；不给默认 50） |
| time | 毫秒 | `>0`：**阻塞到跑完、自动停车**才返回；`0`/不给：立刻返回，**车不停**（要自己再发 `stop`） |
| distance | **厘米** | 仅 `up`/`down`。走这么远（ESP32 闭环），双向校验 |
| angle | **度** | 仅 `left`/`right`。转这么多度 |

响应**只有完成标志**（客户端不用为失败另写一套解析）：

```jsonc
{"completed": true}                                            // 200
{"completed": false, "error": "unsupported action: forward"}    // 400，动作名写错
{"status": "error", "message": "angle 仅对 left/right 动作有效"} // 400，参数配错动作
```

- `speed` 的别名注意：**没有 `forward`**，前进叫 `up`。
- **两种失败形状不同**（实测）：动作名不认识 = `{"completed":false,"error":"unsupported action: xxx"}`；
  `distance`/`angle` 配错动作 = `{"status":"error","message":"…"}`
  （`distance` 仅 `up/down`、`angle` 仅 `left/right`）。都带 400。
- **`distance`/`angle` 是阻塞的**（等固件闭环回报），大距离能挂几秒~几十秒；客户端**别设短超时**，
  服务端自己 30 秒兜底。
- `grab`/`release` 是臂动作，服务端会等那套约 3.5 秒的序列做完再回 `completed`。

---

## 2. `GET /api/motor/direct` —— 直接给左右轮速度

```
GET /api/motor/direct?left=25&right=-25&duration=2
```

| 参数 | 单位 | 说明 |
|------|------|------|
| left / right | % | **±100**（负数 = 反转）。**超范围静默截到 ±100，不报错** |
| duration | 秒 | `>0`：阻塞到时长结束、自动停车后才返回；`0`/不给：立刻返回，**车不停** |

```jsonc
// duration > 0
{"status":"success","left":25,"right":-25,"duration":10,"completed":true,"mode":"completed",
 "left_speed":0.1363,"right_speed":-0.0974}      // 后两个是**实测**轮速 m/s

// duration = 0
{"status":"success","left":0,"right":0,"left_speed":0,"right_speed":0}
```

**板上实测**（2026-09-21）：

| 命令 | 结果 |
|---|---|
| `L=R=25&duration=3` | 全程 0.05→0.10 m/s，3 秒后自动停 |
| `L=+25 R=-25&duration=10` | 原地转，**全程保持**到第 10 秒，到点滑行停 |
| `L=-30 R=+30&duration=1` | 反向（实测 `left_speed` 为负） |
| `left=150` | 实际按 100 跑（0.56 m/s），**响应回显的仍是 150** |

> **两个要记住的点**：
> 1. 响应里的 `left`/`right` 是你**发的原值**，不是生效值（超范围会被静默截断）；
>    要生效值看 `left_speed`/`right_speed`（实测）或 `/api/motor/status` 的 `*_target`。
> 2. **不要指望服务端替你保活**：`duration=10` 的请求会阻塞 10 秒，但期间**不重发**速度命令。
>    实测这块板子上单发一条能连续转 10 秒以上（代码注释里"只转 0.2~0.9s"的旧说法已不成立），
>    所以外部控制器 1 秒发一条足够；要更稳就自己定时重发。

---

## 3. `GET /api/motor/status` —— 前端显示用

```json
{"left_speed":0.0649,"right_speed":0.0844,"left_target":150,"right_target":0,
 "gripper_status":"unknown","gripper_target":0,
 "motor":{"backend":"tt_pid","state":"connected","connected":true,"enabled":true,"attempts":0,"error":""}}
```

| 字段 | 含义 |
|------|------|
| `left_speed` / `right_speed` | **实测**轮速，**m/s**（有符号：负 = 反转）。启动瞬间是 0，等 0.2~0.5 秒再看 |
| `left_target` / `right_target` | 最近一次**目标**速度（%），你发什么它就记什么（含超范围的原值） |
| `motor.state` | `connected` / `reconnecting` / `disabled`（自动重连代理的状态） |
| `motor.connected` | 底盘是否真在线。**掉线时运动指令被丢弃**（不再有 mock 兜底） |
| `motor.attempts` / `motor.error` | 重连次数 / 最近一次错误（排障用） |
| `gripper_status` / `gripper_target` | 夹爪状态（`unknown` = 还没读到）/ 目标位置 |

**轮询建议**：状态采集是 10Hz，界面用 200~500ms 轮询足够；要跟手就用 WS 推送（§5），
别用 50ms 轮询（单核板子会被拖慢）。

---

## 4. `GET/POST /api/config/speed` —— 界面上的"默认速度"

```jsonc
GET  /api/config/speed        → {"forward_speed":55,"turn_speed":40}
POST /api/config/speed  body: {"forward_speed":55,"turn_speed":40}
```

存在 `$AKA_HOME/speed_config.json`。

> **后端不做任何运动决策时会用它**：它只是"界面默认值"的存储。
> 现在的前端是 `BaseControlPage` 拿 `forward_speed`（并 clamp 到 ≤60）当摇杆的初始速度、
> `SpeedConfigPage` 拿它当设置页的初值 —— **发指令时速度是显式带的**（WS/HTTP 参数），
> 所以改了这里不会影响已经发出去的指令，也不会影响 demo 脚本（脚本的速度来自卡片参数）。

---

## 5. WebSocket `/ws/control` —— 摇杆 / 实时控制（推荐）

一条长连接，双向：**发动作**、**收轮速**。前端现有的摇杆就是这么做的。

**发**（JSON 文本帧）：

```jsonc
{"type":"action","action":"up","speed":40,"time":300}   // time 毫秒；0 = 不停，需要自己发 stop
{"type":"action","action":"stop"}                       // 急停
```

- 服务端回 `{"type":"action","result":{…}}`（`result` 就是 §1 里那些字段）。
- **只认命名动作**（`up/down/left/right/stop/grab/release`）—— **没有**直接给左右轮的通道；
  要差速控制走 HTTP 的 `/api/motor/direct`。

**收**（二进制帧）：

| 首字节 | 内容 |
|---|---|
| `0xBB` | 轮速推送：`[0xBB, left_int16_LE, right_int16_LE]`，单位 **mm/s**，前端除 1000 得 m/s（≈10Hz） |
| `0xDD` | JSON 文本帧（前面 1 字节是 0xDD）：action 结果、`motor_status`（连接状态）、`ip` 等 |

> 摇杆交互的常见坑：拖动时按帧率发 `action`（例如 10Hz）就能跟手；松手必须发一次
> `stop`（`time` 只保证"最长转多久"，不保证松手就停）。

---

## 6. 三种"停车"，别搞混

| 怎么发 | 底层动作 | 效果 |
|---|---|---|
| `/api/control?action=stop` | `brake()` | **急停**（钉住，轮子立刻不动） |
| `/api/motor/direct?left=0&right=0` | `sleep()` | **滑行停**（会溜一点） |
| 带 `time=`/`duration=` 的命令到点 | `sleep()` | 同上，滑行停 |

界面上的"停止"按钮用 `action=stop`（急停）更符合直觉。

---

## 7. 谁优先：人的指令 > 脚本

- 任何运动指令（HTTP 或 WS）都会让**正在跑的 demo 脚本**被判
  `superseded: 被新的运动指令取代（人接管）` 并**立刻中断**、交出控制权 —— 设计如此，
  摇杆一动脚本就停。
- `/api/control`、`/api/motor/direct` **不受"一次只能一个脚本"的 409 限制**：随发随生效。
- 底盘掉线（`motor.connected:false`）时运动指令被丢弃 —— 界面上应该据此禁用摇杆并提示。

---

## 8. 常见坑（都实测过）

| 现象 | 原因 |
|---|---|
| `duration=0`/`time=0` 发完车不停 | 这是"设定速度"语义，不是"走一段"；要停就再发一次（急停用 `action=stop`） |
| 直驱能到 100%，但 demo 脚本最快只到 70% | 两条路的上限不同：`/api/motor/direct` 是 **±100**，脚本/命名动作里宿主会把速度 clamp 到 **±70** |
| 响应里 `left=150` 以为生效了 150 | 超范围被**静默截断**到 100，响应回显原值；看 `left_speed`（实测）或 `status.left_target` |
| 发了速度但 `left_speed` 还是 0 | 轮速是实测值，启动有 0.2~0.5 秒延迟；也可能底盘掉线（看 `motor.connected`） |
| 代码注释说"单发一条只转 0.2~0.9s，要 80ms 重发" | **已过时**：这块板子上单发能连续转 10 秒以上（§2 实测表） |
| `distance=30` 只走了 3cm | 那是老 bug（cm/mm 差 10 倍），早已修；现在 `distance` 就是**厘米** |
| 改了 `/api/config/speed` 车没变快 | 后端不读它（§4），它只是界面默认值的存储 |
| 解析 `{completed,error}` 时遇到 `{status,message}` | `distance`/`angle` 配错动作走的是后者（见 §1），错误处理两条都要覆盖 |
| 摇杆松手车还在走 | 松手要发 `stop`；`time` 只是上限 |

---

## 9. 前端最小示例

```ts
// ① 摇杆：按住时按帧率发，松手发 stop
const ws = new WebSocket(`ws://${location.host}/ws/control`);
ws.binaryType = "arraybuffer";
ws.onmessage = (e) => {
    const dv = new DataView(e.data as ArrayBuffer);
    if (dv.getUint8(0) === 0xBB) {                       // 轮速推送（mm/s）
        const left = dv.getInt16(1, true) / 1000;        // → m/s
        const right = dv.getInt16(3, true) / 1000;
        setWheelSpeed({left, right});
    }
};
const drive = (action: string, speed: number) =>
    ws.send(JSON.stringify({type: "action", action, speed, time: 300}));
const stop = () => ws.send(JSON.stringify({type: "action", action: "stop"}));

// ② 走一段 / 转个角度（阻塞到做完，别设短超时）
await fetch("/api/control?action=up&speed=40&distance=50");     // 前进 50 厘米
await fetch("/api/control?action=left&speed=35&angle=90");      // 左转 90 度

// ③ 差速直驱（自己算闭环）
await fetch("/api/motor/direct?left=30&right=15&duration=2");   // 2 秒后自动停

// ④ 显示
const st = await fetch("/api/motor/status").then(r => r.json());
// st.left_speed / st.right_speed（实测 m/s）、st.motor.connected（掉线提示）
```
