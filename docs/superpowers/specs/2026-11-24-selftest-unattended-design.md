# 自动测试「全自动」模式设计（/self-test）

- 日期：2026-11-24
- 状态：设计已确认，待实现
- 相关仓库：`AKA-00`（仅 frontend）
- 固件改动：无（复用现有 HTTP 接口）

## 1. 背景与目标

现有 `/self-test` 是「混合判定」流程：接口结果自动判，视觉/动作效果逐项弹窗人工确认；动作组默认失败连锁跳过。批量生产/检修时这套流程需要人全程盯着点确认，效率低。

目标：在保留现有手动流程的前提下，新增一个**全自动测试**入口——一次点击后不再有任何弹窗，五组测试一次跑完，失败不中断，结束后自动写入历史。

硬约束：

- 不新增后端接口，不重新编译 `aka-capp`。
- 手动流程（逐项确认、连锁跳过、手动保存）行为保持不变。
- 判定只依据接口成功与否，结果明确标记「无人值守、未人工复核」。

不在本次范围：

- 设备端写日志 / PC 端批量调度（沿用既有「后续」规划）。
- 自动判定距离、角度、转向方向、夹持是否真正成功（接口层不可知）。

## 2. 方案对比

| 方案 | 做法 | 取舍 |
|---|---|---|
| **A. 现有 runner 加 `unattended` 开关（采用）** | 复用同一套测试项与执行器，按开关跳过确认与连锁跳过 | 逻辑单点维护，手动/全自动共享步骤与判定细节，页面只多一个入口 |
| B. 独立 unattended runner | 另写一份串行流程 | 两套流程易漂移（参数、超时、清理逻辑重复） |
| C. 页面自动代答弹窗 | UI 层自动点「通过」 | 弹窗仍会产生、notice 会闪烁，语义混乱；且无法关闭连锁跳过 |

采用 A。

## 3. 页面交互

### 3.1 双入口

`/self-test` 控制栏并排两个按钮：

- **「开始测试」**：现有手动流程不变。
- **「全自动测试」**：一次点击跑完全部五组，中途无任何弹窗/确认。

规则：

- 两者都要求先勾选「安全确认（必选）」，运行中都禁用。
- 运行中只显示当前模式的按钮（禁用，文案「测试进行中…」/「全自动进行中…」）与「中止」。
- 「中止」保留，用于安全兜底；中止后的记录照常自动保存。

### 3.2 全自动前置提示

按钮上方常驻一段提示（运行中收起），内容固定：

> 全自动测试按 **动作 → 摄像头 → 抓球 → 舵机 → WiFi** 顺序一次跑完，无需人工确认，失败不中断，结束后自动保存历史。
> 开始前请：网球放在小车正前方 30–60cm；车前后留出 ≥1m；机械臂周围清空。
> 判定只看接口是否成功，不检查转向方向、距离准确度或夹持效果；结果会标记「无人值守」。
> WiFi 组会切换并保留到 Tsinghua-Dongsheng；请通过本车热点（192.168.4.1）打开本页，否则最后一步可能与本页断开。

### 3.3 选项语义

- 现有开关（含 WiFi / 含舵机 / 动作失败后继续 / 抓球参数 / 速度）**只影响「开始测试」**；全自动固定五组、失败不中断。
- 选项区加一行小字说明上述关系。
- 抓球参数与速度仍被全自动沿用（页面上可调）。

### 3.4 展示

- 空闲态流程卡片（`FLOW_ITEMS`）顺序改为：动作 → 摄像头 → 自动抓网球 → 舵机（可选）→ WiFi（可选），文案补充「全自动模式固定全测」。
- 首页副标题（idle）改为提示两种模式的存在。

## 4. runner 判定规则

`RunnerOptions` 新增 `unattended?: boolean`（默认 false）。全自动运行时：

| 环节 | 手动（现状） | 全自动 |
|---|---|---|
| 动作组连锁跳过 | 某项失败后其余动作项标 `skip`「前一项失败，已跳过」 | **不触发连锁跳过**，剩余动作项继续执行 |
| 成对动作 / 转向 | 两项接口都成功后弹「动作是否到位？」 | 两项接口都成功即 `pass`，否则 `fail`；`auto` 记录判定，`manual` 为 `null` |
| 继电器夹爪 | 抓取成功后弹「是否夹住并抬起？」再放下 | 抓取、放下接口都成功即 `pass`，无确认 |
| 摄像头打开 | 打开 + 首帧到达后弹「画面是否正常？」 | 打开成功 + 8s 内收到首帧即 `pass`；否则 `fail`（错误「未收到画面」） |
| 自动抓网球 | 先弹放球提示（可跳过），`done` 后弹「是否夹起？」 | **不弹放球提示**；`done` 即 `pass`，`failed/aborted` 即 `fail`，90s 超时 `fail` 并停止 demo |
| 舵机往返 | 弹「周围无障碍？」与「三个舵机是否都有动作？」 | 不弹；全部 `preview` 成功 + 逆序复位成功 + 回读角度与原始一致即 `pass`，`manual` 为 `null` |
| WiFi | 纯接口判定 | 同左 |
| 整体状态 | 手动模式不变 | `finished` 或 `aborted`（用户中止）；单个失败不会中止流程 |

实现要点：

- 动作循环：`skipRest` 只在 `!opts.unattended && opts.abortOnFailure` 时置位。
- `runPair` / `runGrab` / `openCameraItem` / `runDemo` / `runArm` 在 `opts.unattended` 时跳过对应 `confirm`/`notice` 调用，直接以自动判定落状态；不改变手动分支。
- 预检（底盘未连接、tennis 模型缺失）语义不变；预检失败仍返回 `aborted`。
- 清理逻辑不变（stop、camera close、舵机 finally 恢复原角度）。
- 单项重测（行内 ↻）保持手动确认流程——它是操作员显式发起的交互动作，不受本次运行模式影响。

### 4.1 执行顺序调整：WiFi 移到舵机之后

WiFi 是唯一可能切断本页连接的一步（切网后访问地址/IP 变化）。为避免它连累后续舵机测试：

- `ITEM_DEFS` 顺序调整为：动作组 → `camera_on` / `camera_off` → `demo_grab` → `arm_roundtrip` → `wifi_scan` / `wifi_connect` / `wifi_verify`。
- `runSelfTest` 的分组执行顺序同步调整。
- 手动模式同样生效（展示与执行顺序一致），不改变各步判定细节。

## 5. 记录与历史

### 5.1 记录结构

`SelfTestRecord` 新增可选字段：

```ts
mode?: "manual" | "unattended";  // 缺省视为 manual（兼容旧记录）
```

手动保存写入 `mode: "manual"`；全自动自动保存写入 `mode: "unattended"`。

### 5.2 自动保存

- 全自动运行结束（`finished` 或 `aborted`，含预检失败）后，立即 `appendRecord` 一条记录：
  - `saved = true`（保存按钮显示已完成，不再可点）；
  - `remark` 默认「无人值守」，用户中止时为「无人值守（中途中止）」，预检失败时为「无人值守（预检失败）」；
  - `items` / `summary` / `speed` / `demo` 取本次运行结果与页面参数。
- 手动流程仍由「保存本次记录」按钮保存，备注可编辑。
- 全自动保存后若再点某项 ↻ 重测，`saved` 重置，可再次手动保存（沿用本次 `mode` 与备注）。
- 保存按钮在全自动自动保存后显示「✓ 已自动保存（无人值守）」。

### 5.3 历史列表

- 每条记录首行显示时间与通过/失败/跳过/未确认计数；`mode === "unattended"` 的记录在时间旁加一个小徽标「全自动」，手动记录不显示。
- 导出 JSON 包含 `mode` 字段。

## 6. 文件改动清单

| 文件 | 改动 |
|---|---|
| `frontend/src/selftest/runner.ts` | `RunnerOptions.unattended`；动作循环连锁跳过门控；五个执行函数加无确认分支；`ITEM_DEFS` 与执行顺序调整（WiFi 后置） |
| `frontend/src/selftest/core.ts` | `SelfTestRecord.mode`；可选的 `UNATTENDED_GROUPS` 常量 |
| `frontend/src/pages/SelfTestPage.tsx` | 「全自动测试」按钮、前置提示、选项小字；`startUnattended()`；`runMode` 状态；`buildRecord`/`saveRecord` 重构与自动保存；文案 |
| `frontend/src/selftest/components/HistoryPanel.tsx` | 「全自动」徽标；自动保存后的按钮文案 |
| `frontend/tests/runner.test.ts` | 新增 unattended 用例（见 §7） |
| `frontend/tests/history.test.ts` | `mode` 序列化往返 |

不修改：

- `cpp/**`（无后端改动）
- 现有接口语义
- 手动流程的判定与确认交互（分组顺序调整见 §4.1 除外）

## 7. 测试计划

`cd frontend && npm run test:selftest`（Node `--experimental-strip-types --test`）。

新增 runner 用例（复用现有 `fakeClient`，扩展 `run()` 支持 `unattended`）：

1. **全自动五组跑完**：`groups` 全五组、`confirm/notice` 一旦被调用就抛错；全部项 `pass`，`state === "finished"`，`manual` 全为 `null`。
2. **动作失败不连锁跳过**：首个控制命令返回失败，其余 7 个动作行仍执行；第一行 `fail`、其余 `pass`；`state === "finished"`。
3. **抓球无放球提示**：`notice` 抛错也不被调用；`demoStatus` 返回 `done` 时 `demo_grab` 直接 `pass`。
4. **摄像头首帧失败**：`cameraFrame` 返回 false 时 `camera_on` `fail`，后续 demo 组继续执行并通过。
5. **舵机无确认且必然恢复**：某次 `preview` 失败 → `arm_roundtrip` `fail`；最后一次 `preview` 为原始值 228，`finally` 写回原始角度；`confirm` 零调用。
6. **执行顺序**：全五组时 `wifi.*` 调用发生在所有 `arm.preview` 之后。
7. **回归**：现有手动用例（连锁跳过、两处确认、人工判定覆盖）保持通过。

历史用例：`appendRecord` 写入带 `mode: "unattended"` 的记录后，`loadHistory` 读回仍包含该字段；无 `mode` 的旧记录可正常读取。

## 8. 构建与部署

```
cd frontend
npx tsc -b
WITH_SCREEN=0 npx vite build
node scripts/sri-retry.mjs
node scripts/gzip-assets.mjs
```

产物写入 `AKA-00/static`。部署为覆盖车机文件，执行前与操作者确认目标车 IP；上传控制台 5 个文件到 `/root/AKA-00/static`（`index.html`、`assets/index.js`、`assets/index.js.br`、`assets/index.js.gz`、`assets/index.css`），并清掉同目录过期的 `index.html.br/.gz` 边车。

## 9. 验收标准

1. `/self-test` 出现「开始测试」与「全自动测试」两个按钮；未勾选安全确认时两者均不可点。
2. 点「全自动测试」后全程无弹窗：动作 → 摄像头 → 抓球 → 舵机 → WiFi 顺序执行，单项失败不中断。
3. 动作组某个动作失败后，后续动作项仍执行（不再出现「前一项失败，已跳过」）。
4. 全自动结束后历史列表自动出现一条带「全自动」徽标、备注「无人值守」的记录；保存按钮显示已自动保存。
5. 中止全自动运行后，记录备注为「无人值守（中途中止）」。
6. 手动「开始测试」流程与现状一致：视觉项待确认、默认失败连锁跳过、手动保存备注。
7. 舵机项在全自动模式下无论成败都恢复原始角度与配置值。
8. 执行顺序中 WiFi 三步在舵机项之后。
9. `npm run test:selftest` 全部通过。

## 10. 风险与已知限制

- 全自动不校验转向方向与距离准确性：左右反接、里程偏差只能靠人工或后续「转向校正」发现；记录中已明确标记无人值守。
- WiFi 组会改写 `/etc/aka-wifi.json`，下次开机自动连接目标网络；不自动恢复原网络（与手动模式一致）。
- 若页面通过车机 STA 网络访问，执行到 WiFi 组后本页与切换网络后的车机失去连接：`wifi_verify` 会因请求失败而判失败，但页面不会重载，记录仍会写入本机 `localStorage`；提示中已要求通过 192.168.4.1 访问。
- 自动抓网球仍依赖现场已摆好网球；没放球时会以 90s 超时记失败，不再像手动模式那样提示。
