# 自动测试「手动模式减确认 + 抓球可选 + 全自动跟随开关」设计（/self-test）

- 日期：2026-11-25
- 状态：设计已确认，待实现
- 相关仓库：`AKA-00`（仅 frontend）
- 固件改动：无（复用现有 HTTP 接口）
- 修订关系：本设计**修改** `2026-11-24-selftest-unattended-design.md` 的 §3.3（选项语义）与 §4 判定表的手动列；其余（全自动入口、自动保存、记录结构）沿用。

## 1. 背景与问题

1. 手动模式「开始测试」的动作组每项都要人工确认（7 个成对动作各弹「动作是否到位？」、抓取/放下弹「是否夹住并抬起？」、舵机弹「周围无障碍？」与「三个舵机都有动作？」），一条线跑下来确认次数过多。
2. 自动抓网球目前是手动/全自动的固定项，无法跳过。
3. 实测问题：全自动模式没有勾选「WiFi 测试」，仍然执行了 WiFi 扫描/连接/验证。

### 1.1 根因（WiFi 问题）

`SelfTestPage.startUnattended()` 把分组写死为 `UNATTENDED_GROUPS = ["motion", "camera", "demo", "arm", "wifi"]`（`core.ts:6`），页面开关（`enableWifi` / `enableArm`）只参与 `start()`（手动）的分组拼接。UI 小字也明确写着「全自动固定包含全部 5 组」——行为与用户预期不符。

## 2. 目标

- 手动模式：动作、抓取/放下、舵机测试改为**接口判定**（与全自动一致）；只有**摄像头打开**与**自动抓网球**保留人工确认。
- 自动抓网球改为可选开关，默认勾选。
- 全自动模式遵循页面开关：动作 + 摄像头必测，抓球 / 舵机 / WiFi 按开关执行。

## 3. 判定规则（runner）

| 测试项 | 手动（改前） | 手动（改后） | 全自动（不变） |
|---|---|---|---|
| 7 组成对动作 | 接口成功后弹「动作是否到位？」 | 不弹；两项接口 `completed` 即 `pass`，`manual=null` | 同左（本就无弹窗） |
| 抓取 → 放下 | 抓取成功后弹「是否夹住并抬起？」再放下 | 不弹；抓取、放下接口都成功即 `pass` | 同左 |
| 舵机往返 | 弹「周围无障碍？」+「三个舵机是否都有动作？」 | **两个都去掉**；preview 全部成功 + 逆序复位成功 + 回读角度与原始一致即 `pass` | 同左 |
| 摄像头 打开 | 打开 + 首帧后弹「画面是否正常？」 | **保留** | 打开成功 + 8s 内首帧即 `pass` |
| 摄像头 关闭 | 接口判定 | 不变 | 不变 |
| 自动抓网球 | 放球提示 + `done` 后弹「是否夹起？」 | **保留**（仅在该组被选中时执行） | 不弹；`done` 即 `pass` |
| WiFi 三小项 | 接口判定 | 不变 | 不变 |

其他规则：

- 手动模式的「动作失败后继续」开关（连锁跳过）保持不变：`skipRest` 仍只在手动且未开「继续」时生效。
- 单项重测（↻）跟随同一规则：动作/舵机接口判定，摄像头/抓球仍确认（`retestItem` 不改判定分支）。
- 舵机执行前的安全门在手动模式也移除；安全由页面顶部的「安全确认（必选）」总开关承担。
- 全自动模式的「失败不中断」「无弹窗」「结束自动保存」全部不变。

## 4. 测试范围与开关（页面）

- 新增状态 `enableDemo`，默认 `true`，UI 新增一行「自动抓网球测试」开关。
- 抽出纯函数 `buildGroups`（放 `core.ts`，便于单测）：

```ts
export interface GroupOptions {
  demo: boolean;
  arm: boolean;
  wifi: boolean;
}

/** 动作 + 摄像头必测；其余按开关；顺序固定，WiFi 最后 */
export function buildGroups(opts: GroupOptions): Group[] {
  const groups: Group[] = ["motion", "camera"];
  if (opts.demo) groups.push("demo");
  if (opts.arm) groups.push("arm");
  if (opts.wifi) groups.push("wifi");
  return groups;
}
```

- `start()` 与 `startUnattended()` 都用 `buildGroups({demo: enableDemo, arm: enableArm, wifi: enableWifi})`；删除 `UNATTENDED_GROUPS` 常量及其引用。
- 执行顺序仍由 `runSelfTest` 的固定顺序（动作 → 摄像头 → 抓球 → 舵机 → WiFi）决定，`groups.includes(...)` 只做开关；因此 WiFi 永远最后。
- 说明：全自动模式同样会先执行 `motorConnected()` 预检；`demoHasTennis()` 仅在选择抓球组时调用（现有逻辑）。

## 5. 文案与展示

- 选项区底部小字：
  - 改前：「以上开关只影响『开始测试』；『全自动测试』固定包含全部 5 组、失败不中断。」
  - 改后：「以上开关对『开始测试』和『全自动测试』都生效；动作与摄像头为必测项，全自动失败不中断。」
- `FLOW_ITEMS`：
  - 舵机：去掉「（全自动固定包含）」；
  - WiFi：去掉「（全自动固定包含）」；
  - 自动抓网球：补充「可在选项里关闭」。
- 全自动提示卡：
  - 「按 动作 → 摄像头 → 抓球 → 舵机 → WiFi 一次跑完」改为「按所选项目一次跑完（动作 → 摄像头 → 可选的抓球 → 可选的舵机 → 可选的 WiFi）」；
  - 「只按接口成功判定」等其余描述不变；WiFi 提示仅在启用 WiFi 时已有琥珀提示，保留。
- 首页 idle 副标题：「勾选安全确认后选择『开始测试』（逐项人工确认）或『全自动测试』（五组跑完、自动保存）」→「……『开始测试』（摄像头/抓球人工确认）或『全自动测试』（全接口判定、自动保存）」。

### 5.1 记录与历史

- `SelfTestRecord.mode`、全自动自动保存、备注文案均不变。
- 手动模式因动作项不再确认，记录中的 `manual` 字段对动作/舵机为 `null`，`auto` 仍记录接口判定结果。

## 6. 涉及文件

| 文件 | 变更 |
|---|---|
| `frontend/src/selftest/core.ts` | 删除 `UNATTENDED_GROUPS`；新增 `GroupOptions` + `buildGroups` |
| `frontend/src/selftest/runner.ts` | `runPair` / `runGrab` / `runArm` 删除手动确认分支（`manual` 恒为 `null`） |
| `frontend/src/pages/SelfTestPage.tsx` | `enableDemo` 开关 + 两种模式共用 `buildGroups`；删除 `UNATTENDED_GROUPS` 引用；文案更新 |
| `frontend/tests/core.test.ts` | 用 `buildGroups` 用例替换 `UNATTENDED_GROUPS` 用例 |
| `frontend/tests/runner.test.ts` | 调整手动模式既有用例（不再期待动作/抓取/舵机 confirm）；新增回归用例 |

## 7. 测试与验收

### 7.1 单元测试（`npm run test:selftest`）

- `buildGroups`：
  - 默认 `{demo:true, arm:false, wifi:false}` → `["motion","camera","demo"]`；
  - 全开 → `["motion","camera","demo","arm","wifi"]`（WiFi 最后）；
  - `{demo:false, arm:false, wifi:false}` → `["motion","camera"]`；
  - 未勾 WiFi 时结果不含 `"wifi"`（WiFi 问题回归）。
- `runner`：
  - 手动模式成对动作：`confirm` 调用次数为 0，接口成功后 `status=pass`、`manual=null`；
  - 手动模式抓取/放下：`confirm` 为 0；
  - 手动模式舵机：`confirm` 为 0，`status` 由 preview/恢复/回读决定；
  - 手动模式摄像头：`confirm` 恰好 1 次；抓球：`notice`+`confirm` 仍触发；
  - 既有全自动用例保持通过。

### 7.2 构建

- `npx tsc -b`（类型检查）；
- `WITH_SCREEN=0 npx vite build`（noscreen，outDir=`../static`）+ `node scripts/sri-retry.mjs` + `node scripts/gzip-assets.mjs`（按既有构建流程）。

### 7.3 真机验收

- 手动模式（只勾「安全确认」）：动作组不再逐项弹窗；摄像头与抓球各确认一次。
- 未勾 WiFi 时跑全自动：日志/明细中 WiFi 三小项为 `skip`，不会发起扫描/连接。
- 勾选 WiFi 时全自动仍最后执行 WiFi。

## 8. 不在本次范围

- 设备端写日志、PC 端批量调度。
- 自动判定距离 / 角度 / 转向方向 / 夹持是否真正成功（接口层不可知）。
- OTA 打包与批量分发（构建产物按既有部署流程另行处理）。
