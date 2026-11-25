# 自动测试「手动减确认 + 抓球可选 + 全自动跟随开关」实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 手动模式的动作/抓取/舵机改为接口判定（仅摄像头与自动抓球保留人工确认）；自动抓球改为可选（默认开）；全自动模式改为跟随页面开关（动作+摄像头必测）。

**Architecture:** 纯前端改造。`core.ts` 新增 `buildGroups` 并删除 `UNATTENDED_GROUPS`；`runner.ts` 删除三处手动确认分支；`SelfTestPage.tsx` 两种入口共用 `buildGroups`，新增 `enableDemo` 开关并更新文案。判定语义与全自动已有的接口判定对齐，不新增任何后端接口。

**Tech Stack:** TypeScript / React 19 / Node 22 `--experimental-strip-types --test` / Vite（noscreen 构建到 `../static`）。

**Spec:** `docs/superpowers/specs/2026-11-25-selftest-judgement-simplify-design.md`

**执行约定**
- 所有命令在 `AKA-00/frontend` 下执行（Bash/Git Bash）。
- 当前仓库分支 `main`（沿用既有直接提交约定）；`static/` 被 `.gitignore` 忽略，构建产物不入库。
- 每个 Task 完成后按步骤提交；若用户未明确要求提交，执行时先与用户确认。

---

## 文件结构

| 文件 | 责任 |
|---|---|
| `frontend/src/selftest/core.ts` | 纯逻辑：分组规则 `buildGroups`（动作+摄像头必测，WiFi 最后） |
| `frontend/src/selftest/runner.ts` | 执行与判定：动作/抓取/舵机只按接口结果落状态 |
| `frontend/src/pages/SelfTestPage.tsx` | 页面状态与入口：`enableDemo` 开关、两种模式共用分组、文案 |
| `frontend/tests/core.test.ts` | `buildGroups` 单测（含"未选 WiFi 不跑 WiFi"回归） |
| `frontend/tests/runner.test.ts` | 手动模式确认次数与 `manual=null` 的回归用例 |

---

### Task 1: `buildGroups` 分组规则

**Files:**
- Modify: `frontend/src/selftest/core.ts:4-6`
- Test: `frontend/tests/core.test.ts:3-14`（import）、`frontend/tests/core.test.ts:106-108`（替换旧用例）

- [ ] **Step 1: 写失败测试**

把 `frontend/tests/core.test.ts` 顶部的 import 改为（去掉 `UNATTENDED_GROUPS`，加 `buildGroups`）：

```ts
import {
  DEMO_DEFAULTS,
  MOTION_ROWS,
  buildControlUrl,
  buildGroups,
  classifyControl,
  normalizeDemoParams,
  planArmSweep,
  sameAngles,
  summarizeItems,
  type ItemResult,
} from "../src/selftest/core.ts";
```

把文件末尾的 `UNATTENDED_GROUPS` 用例（现有 106-108 行）整段替换为：

```ts
test("buildGroups: 动作与摄像头必测，其余按开关，WiFi 最后", () => {
  assert.deepEqual(buildGroups({demo: false, arm: false, wifi: false}), ["motion", "camera"]);
  assert.deepEqual(buildGroups({demo: true, arm: false, wifi: false}), ["motion", "camera", "demo"]);
  assert.deepEqual(
    buildGroups({demo: true, arm: true, wifi: true}),
    ["motion", "camera", "demo", "arm", "wifi"],
  );
  assert.equal(buildGroups({demo: false, arm: false, wifi: false}).includes("wifi"), false);
});
```

- [ ] **Step 2: 运行测试确认失败**

Run: `npm run test:selftest`
Expected: FAIL —— `buildGroups` 未导出（TypeError / 语法错误），且 core.test.ts 仍导入已存在的 `UNATTENDED_GROUPS`。

- [ ] **Step 3: 最小实现**

把 `frontend/src/selftest/core.ts` 第 5-6 行：

```ts
/** 全自动模式固定包含的测试组：WiFi 放最后，避免切网影响舵机测试 */
export const UNATTENDED_GROUPS: Group[] = ["motion", "camera", "demo", "arm", "wifi"];
```

替换为：

```ts
export interface GroupOptions {
  demo: boolean;
  arm: boolean;
  wifi: boolean;
}

/** 动作 + 摄像头必测；其余按开关；顺序固定，WiFi 最后（避免切网影响舵机测试） */
export function buildGroups(opts: GroupOptions): Group[] {
  const groups: Group[] = ["motion", "camera"];
  if (opts.demo) groups.push("demo");
  if (opts.arm) groups.push("arm");
  if (opts.wifi) groups.push("wifi");
  return groups;
}
```

- [ ] **Step 4: 运行测试确认通过**

Run: `npm run test:selftest`
Expected: PASS（core / history / runner 全部通过；runner 用例此时仍是旧语义，稍后 Task 2 更新）。

- [ ] **Step 5: 提交**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00"
git add frontend/src/selftest/core.ts frontend/tests/core.test.ts
git commit -m "feat(selftest): 新增 buildGroups，测试组由开关决定"
```

---

### Task 2: runner 删除动作/抓取/舵机的手动确认

**Files:**
- Modify: `frontend/src/selftest/runner.ts:169-239`（`runPair` / `runGrab`）、`frontend/src/selftest/runner.ts:450-543`（`runArm`）
- Test: `frontend/tests/runner.test.ts`

- [ ] **Step 1: 写失败测试**

在 `frontend/tests/runner.test.ts` 的「动作组：7 对 + 抓取按顺序执行，全部通过」用例之后，插入：

```ts
test("手动模式：动作与抓取不弹确认，manual 为空", async () => {
  let confirmCalls = 0;
  const {outcome} = await run(["motion"], {}, {
    confirm: () => { confirmCalls += 1; return true; },
  });
  assert.equal(confirmCalls, 0);
  const items = motionItems(outcome.items);
  assert.equal(items.every(item => item.status === "pass"), true);
  assert.equal(items.every(item => item.manual === null), true);
});

test("手动模式：摄像头与自动抓球仍各弹一次确认", async () => {
  const seen: string[] = [];
  const {outcome} = await run(["camera", "demo"], {}, {
    confirm: (id) => { seen.push(id); return true; },
  });
  assert.deepEqual(seen, ["camera_on", "demo_grab"]);
  assert.equal(outcome.items.every(item => item.status === "pass"), true);
});
```

把「舵机组：三个舵机逐个大幅摆动，人工确认后逆序恢复」用例替换为（名字与断言更新）：

```ts
test("舵机组：三个舵机逐个大幅摆动并逆序恢复，不弹确认", async () => {
  const previews: Array<{key: string; value: number}> = [];
  const writes: Record<string, unknown>[] = [];
  let confirms = 0;
  const {outcome} = await run(["arm"], {
    armPreview: async (_driver, key, value) => {
      previews.push({key, value});
      return {ok: true};
    },
    armWrite: async (_driver, next) => {
      writes.push(structuredClone(next));
      return {ok: true};
    },
  }, {
    confirm: () => { confirms += 1; return true; },
  });
  const item = outcome.items.find(entry => entry.id === "arm_roundtrip")!;
  assert.equal(item.status, "pass");
  assert.equal(item.manual, null);
  assert.deepEqual(previews, [
    {key: "grab_position.servo0", value: 188},
    {key: "grab_position.servo1", value: 126},
    {key: "gripper_open", value: 45},
    {key: "gripper_open", value: 135},
    {key: "grab_position.servo1", value: 86},
    {key: "grab_position.servo0", value: 228},
  ]);
  assert.equal(confirms, 0);
  assert.equal(writes.length, 1);  // finally 写回原始配置
  assert.deepEqual(writes[0], ARM_ANGLES());
});
```

- [ ] **Step 2: 运行测试确认失败**

Run: `npm run test:selftest`
Expected: FAIL —— 「动作与抓取不弹确认」确认次数 > 0；「摄像头与自动抓球仍各弹一次确认」顺序/次数与旧实现一致（可能在动作用例已失败）；舵机用例 `confirms` 为 2。

- [ ] **Step 3: 实现 —— `runPair` 去掉确认**

把 `runPair` 末尾：

```ts
  const firstOk = await runCmd(first);
  const secondOk = await runCmd(second);

  const auto: "pass" | "fail" = firstOk && secondOk ? "pass" : "fail";
  let status: ItemStatus = auto;
  let manual: "pass" | "fail" | null = null;
  if (auto === "pass" && !opts.unattended) {
    patch(id, {status: "confirm", auto});
    manual = await opts.confirm(id, "动作是否到位？（距离 / 角度）") ? "pass" : "fail";
    status = manual;
  }
  patch(id, {status, auto, manual, elapsedMs: now() - started});
  return status === "pass";
```

改为：

```ts
  const firstOk = await runCmd(first);
  const secondOk = await runCmd(second);

  const auto: "pass" | "fail" = firstOk && secondOk ? "pass" : "fail";
  patch(id, {status: auto, auto, manual: null, elapsedMs: now() - started});
  return auto === "pass";
```

- [ ] **Step 4: 实现 —— `runGrab` 去掉确认**

把 `runGrab` 中从 `let held: boolean | null = null;` 到函数末尾：

```ts
  let held: boolean | null = null;
  if (grab.ok && !opts.unattended) {
    patch(id, {status: "confirm"});
    held = await opts.confirm(id, "是否夹住并抬起？");
  }

  const release = await opts.client.control("release", {speed: opts.speed, timeoutMs: 5000});
  steps.push({name: "放下", ok: release.ok, elapsedMs: release.elapsedMs, error: release.error});

  const auto: "pass" | "fail" = grab.ok && release.ok ? "pass" : "fail";
  const manual: "pass" | "fail" | null = held === null ? null : held ? "pass" : "fail";
  const status: ItemStatus = auto === "pass" && held !== false ? "pass" : "fail";
  patch(id, {
    status,
    auto,
    manual,
    steps: steps.map(step => ({...step})),
    elapsedMs: now() - started,
    error: grab.error ?? release.error,
  });
  return status === "pass";
```

改为：

```ts
  const release = await opts.client.control("release", {speed: opts.speed, timeoutMs: 5000});
  steps.push({name: "放下", ok: release.ok, elapsedMs: release.elapsedMs, error: release.error});

  const auto: "pass" | "fail" = grab.ok && release.ok ? "pass" : "fail";
  patch(id, {
    status: auto,
    auto,
    manual: null,
    steps: steps.map(step => ({...step})),
    elapsedMs: now() - started,
    error: grab.error ?? release.error,
  });
  return auto === "pass";
```

- [ ] **Step 5: 实现 —— `runArm` 去掉两个确认**

删除「开跑前安全门」整段：

```ts
  const allowed = opts.unattended
    ? true
    : await opts.confirm(id, "确认机械臂周围无障碍，开始逐个转动底座 / 腕部 / 夹爪？");
  if (!allowed) {
    patch(id, {status: "skip", steps: steps.map(step => ({...step})), error: "操作员取消"});
    return original;
  }
```

删除「动作结果确认」整段：

```ts
  let observed = true;
  if (!opts.unattended) {
    patch(id, {status: "confirm"});
    observed = await opts.confirm(id, "底座 / 腕部 / 夹爪是否都有动作？");
    steps.push({name: "人工确认三个舵机动作", ok: observed, elapsedMs: 0});
    flush();
  }
```

把 `runArm` 末尾：

```ts
  const auto: "pass" | "fail" = movesOk && restored && configOk ? "pass" : "fail";
  const allOk = auto === "pass" && (opts.unattended || observed);
  const error = !restored || !configOk
    ? "舵机恢复失败，请检查角度参数"
    : (movesOk && observed ? undefined : "舵机动作未全部确认");
  patch(id, {
    status: allOk ? "pass" : "fail",
    auto,
    manual: opts.unattended ? null : observed ? "pass" : "fail",
    steps: steps.map(step => ({...step})),
    elapsedMs: now() - started,
    error,
  });
  return original;
```

改为：

```ts
  const auto: "pass" | "fail" = movesOk && restored && configOk ? "pass" : "fail";
  const error = !restored || !configOk
    ? "舵机恢复失败，请检查角度参数"
    : movesOk ? undefined : "舵机动作未全部成功";
  patch(id, {
    status: auto,
    auto,
    manual: null,
    steps: steps.map(step => ({...step})),
    elapsedMs: now() - started,
    error,
  });
  return original;
```

- [ ] **Step 6: 运行测试确认通过**

Run: `npm run test:selftest`
Expected: PASS（含既有全自动用例、单项重测用例）。

- [ ] **Step 7: 提交**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00"
git add frontend/src/selftest/runner.ts frontend/tests/runner.test.ts
git commit -m "feat(selftest): 动作/抓取/舵机改接口判定，不再逐项人工确认"
```

---

### Task 3: 页面开关与文案（抓球可选、全自动跟随开关）

**Files:**
- Modify: `frontend/src/pages/SelfTestPage.tsx`

- [ ] **Step 1: import 改用 `buildGroups`**

`SelfTestPage.tsx:13-23` 的 core 导入中，把 `UNATTENDED_GROUPS` 换成 `buildGroups`，并删除已不再使用的 `type Group`：

```ts
import {
  DEMO_DEFAULTS,
  buildGroups,
  normalizeDemoParams,
  summarizeItems,
  type DemoParams,
  type ItemResult,
  type RunState,
  type SelfTestRecord,
} from "../selftest/core.ts";
```

- [ ] **Step 2: 新增 `enableDemo` 状态（默认开）**

在 `const [enableArm, setEnableArm] = useState(false);` 之后加一行：

```ts
  const [enableDemo, setEnableDemo] = useState(true);
```

- [ ] **Step 3: 手动入口改用 `buildGroups`**

`start()` 中把：

```ts
    const groups: Group[] = ["motion", "camera", "demo"];
    if (enableWifi) groups.push("wifi");
    if (enableArm) groups.push("arm");
```

改为：

```ts
    const groups = buildGroups({demo: enableDemo, arm: enableArm, wifi: enableWifi});
```

- [ ] **Step 4: 全自动入口改用 `buildGroups`（修复 WiFi 问题）**

`startUnattended()` 中把 `groups: [...UNATTENDED_GROUPS],` 改为：

```ts
      groups: buildGroups({demo: enableDemo, arm: enableArm, wifi: enableWifi}),
```

- [ ] **Step 5: 选项区新增「自动抓网球测试」开关**

在「安全确认」行后的分隔线（`SelfTestPage.tsx:590`）与「WiFi 测试」`OptionRow` 之间插入：

```tsx
          <OptionRow
            icon="🎾"
            title="自动抓网球测试"
            desc="识别并抓取网球（需要放球）；关闭后不测该项"
            on={enableDemo}
            disabled={running}
            onChange={setEnableDemo}
          />
```

- [ ] **Step 6: 更新选项说明小字**

把：

```tsx
            以上开关只影响「开始测试」；「全自动测试」固定包含全部 5 组、失败不中断。
```

改为：

```tsx
            以上开关对「开始测试」和「全自动测试」都生效；动作与摄像头为必测项，全自动失败不中断。
```

- [ ] **Step 7: 更新流程卡片与全自动提示卡文案**

`FLOW_ITEMS`（`SelfTestPage.tsx:56-62`）改为：

```ts
const FLOW_ITEMS = [
  {icon: "🚗", title: "动作", desc: "前进/后退 10cm 与 2s，左右转向 30/60/90/180° 与 2s，抓取/放下"},
  {icon: "📷", title: "摄像头", desc: "打开摄像头、行内预览画面、关闭并确认"},
  {icon: "🎾", title: "自动抓网球（可选）", desc: "提示放球后启动视觉抓取脚本；可在选项里关闭"},
  {icon: "🦾", title: "舵机角度（可选）", desc: "底座 / 腕部 / 夹爪逐个大幅转动，最后恢复原始参数"},
  {icon: "📶", title: "WiFi（可选）", desc: "扫描并连接 Tsinghua-Dongsheng，不恢复原网络"},
];
```

全自动提示卡（`SelfTestPage.tsx:728-731`）第一行与 WiFi 行改为：

```tsx
              <div>按所选项目一次跑完（动作 → 摄像头 → 可选的抓球 → 可选的舵机 → 可选的 WiFi），中途无确认，失败不中断，结束自动保存历史。</div>
```

```tsx
              <div>勾选 WiFi 时会切换并保留到 Tsinghua-Dongsheng；请通过本车热点 192.168.4.1 打开本页。</div>
```

- [ ] **Step 8: 更新首页 idle 副标题**

把：

```ts
      : runState === "idle" ? "勾选安全确认后选择「开始测试」（逐项人工确认）或「全自动测试」（五组跑完、自动保存）"
```

改为：

```ts
      : runState === "idle" ? "勾选安全确认后选择「开始测试」（摄像头/抓球人工确认）或「全自动测试」（全接口判定、自动保存）"
```

- [ ] **Step 9: 类型检查与单测**

Run: `npx tsc -b`
Expected: 无输出 / 退出码 0。

Run: `npm run test:selftest`
Expected: PASS。

- [ ] **Step 10: 提交**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00"
git add frontend/src/pages/SelfTestPage.tsx
git commit -m "feat(selftest): 抓球可选，全自动跟随开关，更新文案"
```

---

### Task 4: noscreen 构建

**Files:**
- 产物：`AKA-00/static/{index.html,assets/index.js,index.js.br,index.js.gz,index.css}`（`static/` 已 gitignore）

- [ ] **Step 1: 构建（Git Bash 下手动跑，避免 Windows cmd 不支持 `WITH_SCREEN=0` 前缀）**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00/frontend"
npx tsc -b
WITH_SCREEN=0 npx vite build
node scripts/sri-retry.mjs
node scripts/gzip-assets.mjs
```

Expected: vite 输出 `build` 完成，产物写入 `../static`；`sri-retry` 更新 `index.html` 中 `assets/index.js?v=<hash>` 与 `index.css?v=<hash>`。

- [ ] **Step 2: 核对产物**

```bash
ls -l ../static/index.html ../static/assets/index.js ../static/assets/index.js.br ../static/assets/index.js.gz ../static/assets/index.css
grep -oE 'assets/index\.(js|css)\?v=[0-9a-f]+' ../static/index.html
```

Expected: 5 个文件都存在且时间戳为刚才；`?v=` 与旧值不同（破 `/assets/**` 10 天缓存）。

- [ ] **Step 3: 提交**

本 Task 无源码改动（`static/` 被忽略），无需提交。

---

### Task 5: 部署车机与真机验收（需要一台在线小车）

**Files:**
- 上传脚本：`E:/Project/工作空间/辰龙AI教育机器人/小车实验/car-ai-senses/.debug/upload_verify.py`
- 上传清单：同目录 `console-files.txt`（5 个文件）

- [ ] **Step 1: 上传并逐文件校验 SHA-256**

把 `<car_ip>` 换成当前在线小车内网 IP（root/`chenlong` 已在脚本默认值内）：

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00"
MSYS_NO_PATHCONV=1 CAR_HOST=<car_ip> python \
  "E:/Project/工作空间/辰龙AI教育机器人/小车实验/car-ai-senses/.debug/upload_verify.py" \
  static /root/AKA-00/static \
  index.html assets/index.js assets/index.js.br assets/index.js.gz assets/index.css
```

Expected: 5 行 `ok ...`，最后 `== ok 5 bad 0`。
（`MSYS_NO_PATHCONV=1` 必须加，否则 Git Bash 会把 `/root/...` 改写成 `D:/Git/root/...`。）

- [ ] **Step 2: HTTP 复核**

```bash
curl -s "http://<car_ip>/assets/index.js?v=<新hash>" -o /dev/null -w '%{http_code} %{size_download}\n'
curl -s "http://<car_ip>/self-test" -o /dev/null -w '%{http_code}\n'
```

Expected: `200`；下载大小与本地 `static/assets/index.js` 一致。

- [ ] **Step 3: 真机验收（操作员执行）**

1. 手机/电脑打开 `http://<car_ip>/self-test`，只勾「安全确认」，点「开始测试」：
   - 动作组 8 项**不再弹「动作是否到位」/「是否夹住并抬起」**，逐项显示"运行中 → 通过/失败"；
   - 舵机组（若勾选）：**不再弹安全门与动作确认**；
   - 摄像头：仍弹一次「画面是否正常？」；自动抓球：仍弹放球提示 + 「是否夹起？」。
2. 关掉「自动抓网球测试」开关，点「开始测试」：结果列表里 `自动抓网球` 为「跳过」，且不会启动 demo。
3. **不勾 WiFi**，点「全自动测试」：完成后 WiFi 三小项均为「跳过」，全程无 WiFi 扫描/连接（修复回归）。
4. 勾选 WiFi 再跑全自动：WiFi 三小项最后执行并通过（注意会切网，需从本车热点访问）。

Expected: 以上 4 点全部符合；结束后手动/全自动记录分别正常保存到历史。

- [ ] **Step 4: 记录验收结果**

把验收结果与车号/IP 追加到本计划末尾「验收记录」，并写 session-log。

---

## 自检

- 覆盖 spec：手动减确认（Task 2）、摄像头/抓球保留（Task 2 用例）、抓球可选（Task 3）、全自动跟随开关（Task 1+3）、WiFi 回归用例（Task 1）、文案（Task 3）、构建/部署/验收（Task 4-5）。
- 无占位符：IP 以 `<car_ip>` 运行时参数给出，命令完整。
- 类型/命名一致：`buildGroups(opts: GroupOptions)`、`enableDemo`、`GroupOptions` 在 core 与页面一致；`UNATTENDED_GROUPS` 全仓库删除（core / core.test / SelfTestPage）。

## 验收记录

### 2026-11-25 代码 / 单测 / 部署

- Task 1-3 完成：`npx tsc -b` 无错误；`npm run test:selftest` **39 pass / 0 fail**（新增 `buildGroups`、手动模式不弹确认、摄像头/抓球仍各确认一次等用例）。
- Task 4 构建：noscreen 产物 `static/index.html` 2,915B、`assets/index.js` 376,721B、`.br` 94,650B、`.gz` 111,679B、`index.css` 3,078B；`index.js?v=afc5482c → 238fc910`（CSS 未改，hash 不变）。
- Task 5 部署：车 **172.16.203.25**（MAC `14:0a:02:d9:3d:bd`）上传 5 文件 `== ok 5 bad 0`；HTTP 复核 `/assets/index.js?v=238fc910` 200 / 376,721B，`/self-test` 200，首页引用新 hash。
- 合并 `origin/main`（`53c823f` 网页左右方向切换）后重建并二次部署：`index.js?v=3132f435`（378,475B），5 文件 sha 校验通过、`/self-test` 与 `/speed-config` 均 200。
- 车端 `GET /api/config/motor` 返回 **404**：车上 `aka-capp` 仍是旧二进制（不含 swap_lr 与该校准接口），设置页「左右方向切换」按能力探测整块隐藏；需交叉编译新 capp 的 OTA 包后才会出现。
- 待办：操作员按 Task 5 Step 3 的 4 条清单真机验收（动作不弹确认、抓球关闭跳过、全自动不勾 WiFi 不测 WiFi、勾 WiFi 最后执行）。
