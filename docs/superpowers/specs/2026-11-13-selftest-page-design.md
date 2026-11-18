# 小车自动测试页面设计（/self-test）

- 日期：2026-11-13
- 状态：设计已确认，待实现
- 相关仓库：`AKA-00`（frontend + 板上 capp）
- 固件改动：无（只新增前端页面，复用现有 HTTP 接口）

## 1. 背景与目标

批量生产/检修时需要快速确认每台小车的核心功能是否正常。目前只能逐页手动操作，效率低且结果不可追溯。

目标：在控制台“设置”里增加一个**自动测试页面**，按固定顺序自动执行测试项，现场人员只需观察并在视觉项上确认通过/失败；本次测试结果与备注保存在浏览器本地并可导出。

硬约束：

- 不新增后端接口，不重新编译 `aka-capp`，只用现有 HTTP/WS 接口。
- 测试项 1（动作）、2（摄像头）、3（自动抓网球）默认必测；4（WiFi）、5（舵机角度）可勾选。
- 测试期间有人员在场，视觉类判定由人完成。

不在本次范围：

- 把测试历史写到设备文件/串口日志（当前固件没有可写日志接口，需要新增接口并重新编译 capp）。
- 多车并发批量执行器（页面按单车运行；批量靠人员逐台切换）。

## 2. 方案对比

| 方案 | 做法 | 取舍 |
|---|---|---|
| **A. 混合判定（采用）** | 页面自动串行执行，接口结果自动判；视觉项等待人工点“通过/失败” | 自动化程度高且不误报 |
| B. 纯自动 | 接口成功即 PASS，人工只在失败时介入 | 距离/角度/夹持/画面自动判不了，会误报 |
| C. 纯手动分步 | 每项一个按钮，人工点下一步 | 最可控但最慢，失去自动测试意义 |

采用 A。自动结果作为参考，人工判定可覆盖任意一项。

## 3. 页面与入口

- 新页面：`frontend/src/pages/SelfTestPage.tsx`
- 路由：`/self-test`（`App.tsx`，位于现有子页面路由区）
- 入口：`SettingsPage.tsx` 的 `items` 数组追加一项
  - icon：🧪
  - title：自动测试
  - desc：逐项检查动作、摄像头、抓取、网络与舵机
  - path：`/self-test`
- 子页面无底部导航，使用 `<Header title="自动测试" showClose closeTo="/settings" />`。

## 4. 页面信息架构

自上而下：

1. **设备卡**
   - MAC、固件版本、当前 IP、当前时间
   - 来源：`GET /api/system/info`、`GET /api/ota/version`、`GET /api/wifi/ip`
2. **安全确认**
   - 必选复选框：“我已确认车前 1 米内无障碍、机械臂周围无障碍”
   - 未勾选时“开始测试”禁用
3. **测试项选择**
   - 1 动作、2 摄像头、3 自动抓网球：固定勾选且不可取消
   - 4 WiFi、5 舵机角度：默认不勾选
   - 速度输入：默认 25%，范围 10–40%
4. **控制栏**
   - 开始测试 / 中止 / 立即恢复机械臂角度（运行前未抓到原始角度时禁用）
5. **测试清单**
   - 按组显示，每行一个测试项（动作类为成对一行）
   - 行内：名称、参数摘要、状态（待测/运行中/待确认/通过/失败/跳过）、耗时、错误信息
   - 待确认状态显示“通过 / 失败”按钮
   - 失败时展开显示子步骤结果（如“←30° ok / →30° 失败”）
6. **结束区**
   - 备注 textarea
   - 保存本次记录 / 导出 JSON
   - 历史列表（可折叠）：时间、结果汇总、失败项、备注

## 5. 执行状态机

```
idle → precheck → running(item i) → [awaiting_confirm] → running(item i+1) → … → cleanup → finished
                     ↘ failed → abort（默认）或继续（勾选“失败后继续”）
```

- **预检（自动，不计入测试项，约 2 秒）**
  - `GET /api/system/info` → MAC
  - `GET /api/ota/version` → 版本
  - `GET /api/motor/status` → `motor.connected === true`（**唯一硬失败项，不满足直接中止**）
  - `GET /api/camera/status` → 能返回（软检查：失败只提示，不中止）
  - `GET /api/demo/list` → 模型列表包含 `tennis`（软检查：缺失时自动抓网球项直接记失败并跳过）
  - `GET /api/arm/angles` → 能读取（仅在勾选舵机组时检查；读不到则舵机组记失败并跳过）
- **串行队列**：一次只执行一项，前一项结束（含人工确认）后才进入下一项。
- **每项记录**：`startedAt`、`elapsedMs`、请求参数、HTTP 状态、响应体、错误信息、自动判定、人工判定。
- **失败策略**：动作类失败默认中止后续动作项；页面提供“失败后继续”复选框。
- **清理（无论成功失败都执行）**
  1. `GET /api/control?action=stop`
  2. `POST /api/camera/close`
  3. 若勾选舵机组且已保存 `original`，`POST /api/arm/angles` 恢复原角度

## 6. 测试项规格

统一速度：页面速度设置（默认 25%，范围 10–40%），不使用 `config.speed`。

### 6.1 动作组（必测，成对一行）

每行包含两个子步骤，顺序执行；任一子步骤失败该行记失败，但继续执行第二个子步骤（用于区分左右/前后单向故障）。每对执行完毕回到原点/原朝向。

| 行 ID | 子步骤 | 接口 |
|---|---|---|
| `move_cm` | 前进 10cm → 后退 10cm | `GET /api/control?action=up&distance=10&speed=<s>`<br>`GET /api/control?action=down&distance=10&speed=<s>` |
| `move_time` | 前进 2s → 后退 2s | `GET /api/control?action=up&speed=<s>&time=2000`<br>`GET /api/control?action=down&speed=<s>&time=2000` |
| `turn_30` | 左 30° → 右 30° | `GET /api/control?action=left&angle=30&speed=<s>`<br>`GET /api/control?action=right&angle=30&speed=<s>` |
| `turn_60` | 左 60° → 右 60° | 同上，`angle=60` |
| `turn_90` | 左 90° → 右 90° | 同上，`angle=90` |
| `turn_180` | 左 180° → 右 180° | 同上，`angle=180`；页面单独提示需要最大空间 |
| `turn_time` | 左 2s → 右 2s | `GET /api/control?action=left&speed=<s>&time=2000`<br>`GET /api/control?action=right&speed=<s>&time=2000` |
| `grab` | 抓取 → 人工确认夹住 → 放下 | `GET /api/control?action=grab`<br>`GET /api/control?action=release` |

超时：

| 类型 | 前端超时 |
|---|---|
| distance / angle 动作 | 35s（后端上限 30s） |
| time 动作 | `time + 5s` |
| grab | 10s |
| release | 3s |

超时后立即补发 `GET /api/control?action=stop`，再记失败。

判据：

- 自动：HTTP 200 且响应 `completed === true`。
- 人工：距离/角度是否准确、是否真正夹住。
- 响应 `completed === false` 时把 `error` 原文显示在行内。

### 6.2 摄像头组（必测）

| 行 ID | 步骤 | 自动判据 |
|---|---|---|
| `camera_on` | `POST /api/camera/open`，随后页面渲染 `<img src="/api/camera/stream?fps=10">` | `camera_on === true` 且首帧 `onLoad` 在 5s 内触发 |
| `camera_off` | `POST /api/camera/close` → `GET /api/camera/status` | `camera_on === false` |

- 人工确认：画面正常、板载屏点亮并显示画面。
- 可选记录 `GET /api/camera/snapshot` 的 `width/height` 与 base64 字节数（不保存图像内容）。
- 关闭摄像头后必须把 `<img>` 的 `src` 清空，避免长连接残留。

### 6.3 自动抓网球组（必测）

1. 页面提示：“请把网球放在车前 30–60cm”。
2. 启动：
   ```
   POST /api/demo/init
   {"action":"grab","model":"tennis","target_size":280,"speed":20,"turn_speed":18,"mode":"once","wait":false}
   ```
   直接使用 action/model，不依赖 `tennis-grab` 卡片是否存在。
3. 轮询 `GET /api/demo/status`，间隔 1s，展示 `state/message/round`。
4. 超时 90s（可配置）→ `POST /api/demo/stop`，记失败。
5. 人工确认：是否夹起并抬起。
6. 说明：脚本内部会自行打开摄像头；`once` 模式服务端最长 5 分钟，页面必须自行兜底超时。

### 6.4 WiFi 组（可勾选，默认不勾）

| 行 ID | 步骤 | 自动判据 |
|---|---|---|
| `wifi_scan` | `GET /api/wifi/scan`（约 5s） | 返回列表包含 `Tsinghua-Dongsheng`；记录 `signal`、`secured` |
| `wifi_connect` | `POST /api/wifi/connect`，body `{"ssid":"Tsinghua-Dongsheng","password":""}` | 2xx 且返回 `ip` |
| `wifi_verify` | `GET /api/wifi/status` + `GET /api/wifi/ip` | `ssid === "Tsinghua-Dongsheng"` 且 IP 非空 |

- 超时：scan 10s、connect 25s、verify 10s。
- 若 `wifi_connect` 失败，`wifi_verify` 直接记 `skip`，不再请求。
- 页面顶部固定警告：**本次测试必须通过本车热点（192.168.4.1）访问**；若当前是通过车的 STA（wlan1）访问，换网后 IP 变化会导致页面请求断开。
- 连接成功会改写 `/etc/aka-wifi.json`，下次开机自动连接该网络。
- **不恢复原网络**：页面只记录测试前的 SSID 供人工核对，不做自动恢复。

### 6.5 舵机组（可勾选，默认不勾）

| 步骤 | 操作 | 判据 |
|---|---|---|
| 1 | `GET /api/arm/angles` → 保存 `original` | 返回 `{driver, angles}` |
| 2 | 提示确认机械臂周围无障碍 | 必须人工确认 |
| 3 | `POST /api/arm/angles` 下发修改后的完整对象（默认把 `gripper_open` 加一个步长后夹紧到合法范围：ZP10S 步长 10，STS3215 步长 100） | HTTP 200 且返回 angles 含新值 |
| 4 | 停 800ms 后 `GET /api/arm/angles` | 配置值等于修改后的值 |
| 5 | `POST /api/arm/angles` 恢复 `original` | HTTP 200 |
| 6 | `GET /api/arm/angles` 深比较 | 与 `original` 完全一致 |

- 超时：每次请求 5s。
- **恢复必须放在 `finally`**；即使中间步骤抛错也要执行。
- 页面常驻“立即恢复机械臂角度”按钮，使用运行前抓到的 `original`。
- 已知语义：`GET /api/arm/angles` 返回的是配置文件值，不是硬件实测角度；本测试的“记住/恢复”即配置参数往返。

## 7. 历史记录与备注

存储：`localStorage`，key `aka_selftest_history_v1`，值为数组（新记录在前）。

记录结构：

```ts
interface SelfTestRecord {
  id: string;          // `${ts}-${random}`
  ts: number;          // 结束时间戳 ms
  mac: string;
  version: string;
  ip: string;
  speed: number;
  items: {
    id: string;
    name: string;
    group: "motion" | "camera" | "demo" | "wifi" | "arm";
    status: "pass" | "fail" | "skip" | "unconfirmed";
    auto: "pass" | "fail" | null;   // 软件判定
    manual: "pass" | "fail" | null; // 人工判定（可覆盖 auto）
    substeps: { name: string; status: string; elapsedMs: number; error?: string }[];
    elapsedMs: number;
    error?: string;
  }[];
  summary: { pass: number; fail: number; skip: number; unconfirmed: number };
  remark: string;
}
```

规则：

- 未勾选的组记为 `skip`，不算失败。
- 视觉项自动结果仅供参考，最终状态优先取 `manual`；未点确认且自动通过则记 `unconfirmed`，并在汇总中单独计数。
- 一次运行只要存在 `fail` 即为失败；有 `unconfirmed` 时提示“有未确认项”。
- 保存记录时写入备注；历史列表可查看、导出。
- 导出：JSON 文件，文件名 `selftest-<mac>-<yyyyMMdd-HHmmss>.json`（Blob + `a[download]`）。
- 历史按 `mac` 区分。若所有车都通过热点 `192.168.4.1` 访问，则共用同一 origin，历史自然汇总在同一设备上。
- 历史上限：保留最近 100 条，超出丢弃最旧记录。

## 8. 错误处理与安全

- 所有请求统一封装：返回 `{ok, status, data, error, elapsedMs}`；网络错误映射为可读文案。
- 预检失败（底盘未连接等）→ 不进入测试，显示原因与修复建议。
- 动作类超时 → 补发 stop，并中止后续动作项（除非勾选“失败后继续”）。
- 页面卸载/离开路由时：若测试仍在运行，先 `action=stop` 和 `/api/demo/stop`。
- WiFi 测试期间断线：显示“连接可能已切换到新网络”的提示，引导用户在车的 AP 下重新打开页面；不自动跳转。
- 舵机恢复失败：持续显示红色告警，并保留“立即恢复”按钮。

## 9. 文件改动清单

| 文件 | 改动 |
|---|---|
| `frontend/src/pages/SelfTestPage.tsx` | 新增：页面、状态机、测试项执行、人工确认、历史展示 |
| `frontend/src/utils/selftestHistory.ts` | 新增：localStorage 读写、导出 JSON、按 MAC 过滤 |
| `frontend/src/api.ts` | 新增 `motor.control(action, {distance?, angle?, time?, speed?, timeoutMs?})`，返回解析后的 JSON 并检查 HTTP 状态 |
| `frontend/src/App.tsx` | 新增 `/self-test` 路由 |
| `frontend/src/pages/SettingsPage.tsx` | 新增设置项 |

不修改：

- `cpp/**`（无后端改动）
- 其他页面与现有接口语义

## 10. 构建与部署

```
cd frontend
npx tsc -b
WITH_SCREEN=0 npx vite build
node scripts/sri-retry.mjs
node scripts/gzip-assets.mjs
```

部署控制台 5 个文件到 `/root/AKA-00/static`：`index.html`、`assets/index.js`、`assets/index.js.br`、`assets/index.js.gz`、`assets/index.css`。

## 11. 验收标准

1. `/settings` 出现“自动测试”入口，点击进入 `/self-test`。
2. 未勾选安全确认时无法开始。
3. 开始后按顺序执行：预检 → 动作 → 摄像头 → 自动抓网球（→ 可选 WiFi → 可选舵机）→ 清理。
4. 动作项成对执行，每对结束回到原点/原朝向；子步骤结果可见。
5. 视觉项停在“待确认”，人工点通过/失败后继续。
6. 摄像头打开后页面显示实时画面，关闭后状态为 false 且画面移除。
7. 自动抓网球最多 90s 超时，超时自动停止并记失败。
8. WiFi 项能扫描到并连接 `Tsinghua-Dongsheng`（无密码），记录新 IP。
9. 舵机项无论成败都能恢复原始角度，恢复后 GET 值与运行前完全一致。
10. 结束后可填写备注并保存；历史列表可查看并导出 JSON。
11. 底盘未连接时预检直接中止并给出提示。

## 12. 后续（不在本次实现）

- 设备端测试日志：需要新增 `aka-capp` 接口并重新编译（riscv64-musl 工具链 + TPU SDK），届时可把历史写入 `$AKA_HOME/test_history.jsonl`。
- PC 端批量 runner：扫描网段、逐台触发测试并汇总 JSON/CSV；依赖本页面的结果结构。
