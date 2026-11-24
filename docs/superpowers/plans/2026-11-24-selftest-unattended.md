# AKA-00 自动测试「全自动」模式实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `/self-test` 增加「全自动测试」入口：一次点击按 动作 → 摄像头 → 抓球 → 舵机 → WiFi 跑完全部五组，中途无确认、失败不中断，结束后自动写入历史。

**Architecture:** 复用现有 `runSelfTest` 执行器，新增 `unattended` 选项把「确认/放球提示/失败连锁跳过」三处交互关掉，判定只看接口结果；页面新增第二个入口按钮与自动保存逻辑；手动流程保持原样。

**Tech Stack:** React 19 + TypeScript 5.9 + Vite 8；测试用 Node 22 `--experimental-strip-types --test`（`npm run test:selftest`）。

**规格文档：** `docs/superpowers/specs/2026-11-24-selftest-unattended-design.md`

**约定：** 所有路径相对 `AKA-00/` 仓库根目录；命令在 Git Bash 下执行；当前分支 `main`（按仓库惯例直接提交到 main）。本计划只暂存列出的文件，不要 `git add -A`（工作区还有 init.sh 等无关改动）。

---

## 文件结构

| 文件 | 职责 | 改动 |
|---|---|---|
| `frontend/src/selftest/core.ts` | 纯类型与常量 | `SelfTestRecord.mode`、`UNATTENDED_GROUPS` |
| `frontend/src/selftest/runner.ts` | 流程编排与判定 | `unattended` 开关、无确认分支、WiFi 后置 |
| `frontend/src/pages/SelfTestPage.tsx` | 页面状态与入口 | 双按钮、提示、`startUnattended`、自动保存 |
| `frontend/src/selftest/components/HistoryPanel.tsx` | 历史展示 | 「全自动」徽标、自动保存按钮文案 |
| `frontend/tests/core.test.ts` | 纯逻辑测试 | `UNATTENDED_GROUPS` 用例 |
| `frontend/tests/history.test.ts` | 历史存储测试 | `mode` 往返用例 |
| `frontend/tests/runner.test.ts` | 流程测试 | unattended 用例、顺序用例 |

---

### Task 1: 记录类型与全自动组常量

**Files:**
- Modify: `frontend/src/selftest/core.ts`
- Test: `frontend/tests/core.test.ts`, `frontend/tests/history.test.ts`

- [ ] **Step 1: 写失败测试（core）**

在 `frontend/tests/core.test.ts` 的 import 列表里加 `UNATTENDED_GROUPS`：

```ts
import {
  DEMO_DEFAULTS,
  MOTION_ROWS,
  UNATTENDED_GROUPS,
  buildControlUrl,
  classifyControl,
  normalizeDemoParams,
  planArmSweep,
  sameAngles,
  summarizeItems,
  type ItemResult,
} from "../src/selftest/core.ts";
```

文件末尾追加：

```ts
test("UNATTENDED_GROUPS: 覆盖全部五组且 WiFi 最后", () => {
  assert.deepEqual(UNATTENDED_GROUPS, ["motion", "camera", "demo", "arm", "wifi"]);
});
```

- [ ] **Step 2: 运行测试确认失败**

Run（在 `AKA-00/frontend` 下）：

```bash
npm run test:selftest
```

Expected: FAIL，报 `SyntaxError: The requested module '../src/selftest/core.ts' does not provide an export named 'UNATTENDED_GROUPS'`。

- [ ] **Step 3: 实现常量与记录字段**

`frontend/src/selftest/core.ts`，在 `export type Group = ...` 下一行加：

```ts
/** 全自动模式固定包含的测试组：WiFi 放最后，避免切网影响舵机测试 */
export const UNATTENDED_GROUPS: Group[] = ["motion", "camera", "demo", "arm", "wifi"];
```

`SelfTestRecord` 接口末尾（`remark: string;` 之后）加：

```ts
  /** 手动逐项确认 / 全自动无人值守（旧记录缺省视为手动） */
  mode?: "manual" | "unattended";
```

- [ ] **Step 4: 补历史 `mode` 往返测试**

在 `frontend/tests/history.test.ts` 末尾追加：

```ts
test("appendRecord/loadHistory: 保留 mode，旧记录无 mode 仍可读", () => {
  const storage = memoryStorage();
  appendRecord(storage, {...record(1), mode: "unattended"});
  appendRecord(storage, record(2));
  const list = loadHistory(storage);
  assert.equal(list[0].mode, undefined);
  assert.equal(list[1].mode, "unattended");
});
```

- [ ] **Step 5: 运行测试确认全部通过**

Run：

```bash
npm run test:selftest
```

Expected: PASS，`fail 0`。再跑类型检查：

```bash
npx tsc -b
```

Expected: 退出码 0，无输出。

- [ ] **Step 6: 提交**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00"
git add frontend/src/selftest/core.ts frontend/tests/core.test.ts frontend/tests/history.test.ts
git commit -m "feat(selftest): 记录增加 mode 字段与全自动测试组常量"
```

---

### Task 2: runner 无人值守判定

**Files:**
- Modify: `frontend/src/selftest/runner.ts`
- Test: `frontend/tests/runner.test.ts`

- [ ] **Step 1: 扩展测试 harness（先改测试）**

`frontend/tests/runner.test.ts` 里把 `run()` 的 options 类型与调用改为：

```ts
async function run(groups: Group[], overrides: Partial<Client> = {}, options: {
  isAborted?: () => boolean;
  confirm?: (id: string) => boolean;
  notice?: (id: string) => boolean;
  now?: () => number;
  demoParams?: DemoParams;
  unattended?: boolean;
  cameraFrame?: (timeoutMs: number) => Promise<boolean>;
} = {}) {
  const fake = fakeClient();
  Object.assign(fake.client, overrides);
  const outcome = await runSelfTest({
    groups,
    speed: 25,
    demoParams: options.demoParams ?? {target_size: 280, speed: 30, turn_speed: 22},
    abortOnFailure: true,
    unattended: options.unattended,
    client: fake.client,
    cameraFrame: options.cameraFrame ?? (async () => true),
    confirm: async (id) => options.confirm ? options.confirm(id) : true,
    notice: async (id) => options.notice ? options.notice(id) : true,
    isAborted: options.isAborted ?? (() => false),
    onUpdate: () => {},
    onArmSnapshot: () => {},
    onCamera: () => {},
    now: options.now,
  });
  return {fake, outcome};
}
```

在文件末尾（`retest` 相关用例之前或之后均可）追加 5 个用例：

```ts
test("全自动：五组跑完且不调用任何确认/提示", async () => {
  let confirmCalls = 0;
  let noticeCalls = 0;
  const {outcome} = await run(["motion", "camera", "demo", "wifi", "arm"], {}, {
    unattended: true,
    confirm: () => { confirmCalls += 1; return true; },
    notice: () => { noticeCalls += 1; return true; },
  });
  assert.equal(confirmCalls, 0);
  assert.equal(noticeCalls, 0);
  assert.equal(outcome.state, "finished");
  assert.equal(outcome.items.every(item => item.status === "pass"), true);
  assert.equal(outcome.items.every(item => item.manual === null), true);
});

test("全自动：动作失败不连锁跳过，后续动作项继续", async () => {
  let controlCalls = 0;
  const {outcome} = await run(["motion"], {
    control: async () => {
      controlCalls += 1;
      return controlCalls === 1
        ? {ok: false, completed: false, error: "动作失败", elapsedMs: 1}
        : {ok: true, completed: true, elapsedMs: 1};
    },
  }, {unattended: true});
  const items = motionItems(outcome.items);
  assert.equal(items[0].status, "fail");
  assert.deepEqual(items.slice(1).map(item => item.status), Array(7).fill("pass"));
  assert.equal(outcome.state, "finished");
});

test("全自动：抓球不弹放球提示，done 即通过", async () => {
  let noticeCalls = 0;
  const {outcome} = await run(["demo"], {}, {
    unattended: true,
    notice: () => { noticeCalls += 1; return true; },
  });
  assert.equal(noticeCalls, 0);
  assert.equal(outcome.items.find(item => item.id === "demo_grab")!.status, "pass");
});

test("全自动：摄像头首帧超时后继续执行后续组", async () => {
  const {outcome} = await run(["camera", "demo"], {}, {
    unattended: true,
    cameraFrame: async () => false,
  });
  const cameraOn = outcome.items.find(item => item.id === "camera_on")!;
  const demo = outcome.items.find(item => item.id === "demo_grab")!;
  assert.equal(cameraOn.status, "fail");
  assert.equal(demo.status, "pass");
});

test("全自动：舵机无确认，失败后仍恢复原始角度", async () => {
  const previews: number[] = [];
  const writes: Record<string, unknown>[] = [];
  let confirmCalls = 0;
  const {outcome} = await run(["arm"], {
    armPreview: async (_driver, key, value) => {
      previews.push(value);
      return key === "grab_position.servo0" && value === 188
        ? {ok: false, error: "串口无响应"}
        : {ok: true};
    },
    armWrite: async (_driver, next) => {
      writes.push(structuredClone(next));
      return {ok: true};
    },
  }, {
    unattended: true,
    confirm: () => { confirmCalls += 1; return true; },
  });
  const item = outcome.items.find(entry => entry.id === "arm_roundtrip")!;
  assert.equal(item.status, "fail");
  assert.equal(item.manual, null);
  assert.equal(previews[previews.length - 1], 228);
  assert.equal(confirmCalls, 0);
  assert.deepEqual(writes[0], ARM_ANGLES());
});
```

- [ ] **Step 2: 运行测试确认失败**

Run：

```bash
npm run test:selftest
```

Expected: FAIL。典型输出：
- `全自动：五组…` → `confirmCalls` 大于 0（`expected 0, actual 7` 之类）；
- `全自动：动作失败…` → 后续动作项为 `skip` 而非 `pass`；
- `全自动：抓球…` → `noticeCalls` 为 1；
- `全自动：摄像头…` → demo 项状态不符（演示确认弹窗处卡住/超时行为不同）；
- 舵机用例 → `confirmCalls` 为 2。

（`unattended` 字段此时只是被忽略，不会报类型错误——测试用 strip-types 不检查类型。）

- [ ] **Step 3: 实现 runner 无人值守分支**

`frontend/src/selftest/runner.ts`：

3a. `RunnerOptions` 在 `abortOnFailure: boolean;` 后加：

```ts
  /** 全自动（无人值守）：不弹确认/提示、动作失败不连锁跳过，判定只看接口结果 */
  unattended?: boolean;
```

3b. `runSelfTest` 动作循环里的连锁跳过改为：

```ts
        if (!passed && opts.abortOnFailure && !opts.unattended) skipRest = true;
```

3c. `runPair()` 的确认块改为：

```ts
  if (auto === "pass" && !opts.unattended) {
    patch(id, {status: "confirm", auto});
    manual = await opts.confirm(id, "动作是否到位？（距离 / 角度）") ? "pass" : "fail";
    status = manual;
  }
```

3d. `runGrab()` 整段判定改为：

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

3e. `openCameraItem()` 的 `if (open.ok && frame)` 分支改为：

```ts
  if (open.ok && frame) {
    if (opts.unattended) {
      patch("camera_on", {status: "pass", auto: "pass", manual: null, steps, elapsedMs: now() - started});
    } else {
      patch("camera_on", {status: "confirm", auto: "pass", steps});
      const confirmed = await opts.confirm("camera_on", "画面是否正常？（网页与板载屏）");
      patch("camera_on", {
        status: confirmed ? "pass" : "fail",
        auto: "pass",
        manual: confirmed ? "pass" : "fail",
        steps,
        elapsedMs: now() - started,
      });
    }
  } else {
```

3f. `runDemo()` 放球提示改为：

```ts
  const placed = opts.unattended
    ? true
    : (await opts.notice?.(id, "请把网球放在小车正前方 30–60cm 处，并移开周围障碍物")) ?? true;
```

`runDemo()` 末尾的成功分支改为：

```ts
  if (opts.unattended) {
    patch(id, {status: "pass", auto, manual: null, elapsedMs: now() - started});
    return;
  }

  patch(id, {status: "confirm", auto, elapsedMs: now() - started});
  const confirmed = await opts.confirm(id, `是否夹起并抬起？${message ? `（${message}）` : ""}`);
  patch(id, {
    status: confirmed ? "pass" : "fail",
    auto,
    manual: confirmed ? "pass" : "fail",
    elapsedMs: now() - started,
  });
```

3g. `runArm()` 三处：

开始确认改为：

```ts
  const allowed = opts.unattended
    ? true
    : await opts.confirm(id, "确认机械臂周围无障碍，开始逐个转动底座 / 腕部 / 夹爪？");
  if (!allowed) {
    patch(id, {status: "skip", steps: steps.map(step => ({...step})), error: "操作员取消"});
    return original;
  }
```

动作确认改为：

```ts
  let observed = true;
  if (!opts.unattended) {
    patch(id, {status: "confirm"});
    observed = await opts.confirm(id, "底座 / 腕部 / 夹爪是否都有动作？");
    steps.push({name: "人工确认三个舵机动作", ok: observed, elapsedMs: 0});
    flush();
  }
```

最终判定改为：

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

- [ ] **Step 4: 运行测试确认通过**

Run：

```bash
npm run test:selftest
```

Expected: PASS，`fail 0`，其中原有「动作失败：默认跳过后续动作项」「舵机组：…人工确认…」等手动用例仍通过。

- [ ] **Step 5: 提交**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00"
git add frontend/src/selftest/runner.ts frontend/tests/runner.test.ts
git commit -m "feat(selftest): runner 支持全自动无人值守判定"
```

---

### Task 3: 执行顺序 WiFi 后置

**Files:**
- Modify: `frontend/src/selftest/runner.ts`
- Test: `frontend/tests/runner.test.ts`

- [ ] **Step 1: 测试 harness 增加调用记录**

`frontend/tests/runner.test.ts` 的 `fakeClient()` 中，把对应实现替换为下面 6 项（其余不变）：

```ts
    wifiScan: async () => {
      calls.push("wifi.scan");
      return {ok: true, list: [{ssid: "Tsinghua-Dongsheng", signal: -40, secured: false}]};
    },
    wifiConnect: async () => {
      calls.push("wifi.connect");
      return {ok: true, ip: "192.168.9.9"};
    },
    wifiStatus: async () => {
      calls.push("wifi.status");
      return {ok: true, ssid: "Tsinghua-Dongsheng", ip: "192.168.9.9"};
    },
    armRead: async () => {
      calls.push("arm.read");
      return {ok: true, driver: "zp10s", angles: ARM_ANGLES()};
    },
    armWrite: async () => {
      calls.push("arm.write");
      return {ok: true};
    },
    armPreview: async () => {
      calls.push("arm.preview");
      return {ok: true};
    },
```

文件末尾追加：

```ts
test("全自动：WiFi 三步骤排在舵机之后", async () => {
  const {fake} = await run(["motion", "camera", "demo", "wifi", "arm"], {}, {unattended: true});
  const lastArm = fake.calls.lastIndexOf("arm.preview");
  const firstWifi = fake.calls.indexOf("wifi.scan");
  assert.equal(firstWifi >= 0, true);
  assert.equal(lastArm >= 0, true);
  assert.equal(lastArm < firstWifi, true);
});
```

- [ ] **Step 2: 运行测试确认失败**

Run：

```bash
npm run test:selftest
```

Expected: FAIL，`全自动：WiFi 三步骤排在舵机之后` 报 `false !== true`（当前 WiFi 在舵机之前）。

- [ ] **Step 3: 调整 ITEM_DEFS 与执行顺序**

`frontend/src/selftest/runner.ts` 的 `ITEM_DEFS` 改为：

```ts
const ITEM_DEFS: ItemDef[] = [
  ...MOTION_ROWS.map(row => ({id: row.id, name: row.label, group: "motion" as const})),
  {id: "camera_on", name: "摄像头 打开 / 显示", group: "camera"},
  {id: "camera_off", name: "摄像头关闭", group: "camera"},
  {id: "demo_grab", name: "自动抓网球", group: "demo"},
  {id: "arm_roundtrip", name: "舵机角度 调整 / 恢复", group: "arm"},
  {id: "wifi_scan", name: "WiFi 扫描", group: "wifi"},
  {id: "wifi_connect", name: `WiFi 连接 ${WIFI_SSID}`, group: "wifi"},
  {id: "wifi_verify", name: "WiFi 验证", group: "wifi"},
];
```

`runSelfTest()` 中把 WiFi 与 arm 两个执行块对调（arm 在前）：

```ts
    if (!aborted && !checkAbort() && opts.groups.includes("arm")) {
      arm = await runArm(opts, patch, now);
    }

    if (!aborted && !checkAbort() && opts.groups.includes("wifi")) {
      await runWifi(opts, patch, now);
    }
```

- [ ] **Step 4: 运行测试确认通过**

Run：

```bash
npm run test:selftest
```

Expected: PASS，`fail 0`。

- [ ] **Step 5: 提交**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00"
git add frontend/src/selftest/runner.ts frontend/tests/runner.test.ts
git commit -m "feat(selftest): WiFi 组后置到舵机之后，避免切网影响后续测试"
```

---

### Task 4: 页面全自动入口与自动保存

**Files:**
- Modify: `frontend/src/pages/SelfTestPage.tsx`
- Modify: `frontend/src/selftest/components/HistoryPanel.tsx`

本任务无 DOM 测试框架，验证方式为 `npx tsc -b` 编译通过 + `npm run test:selftest` 回归。

- [ ] **Step 1: 导入常量与新增状态**

`SelfTestPage.tsx` 的 core 导入改为：

```ts
import {
  DEMO_DEFAULTS,
  UNATTENDED_GROUPS,
  normalizeDemoParams,
  summarizeItems,
  type DemoParams,
  type Group,
  type ItemResult,
  type RunState,
  type SelfTestRecord,
} from "../selftest/core.ts";
```

在 `const [saved, setSaved] = useState(false);` 后加：

```ts
  const [autoSaved, setAutoSaved] = useState(false);
  const [runMode, setRunMode] = useState<"manual" | "unattended">("manual");
```

- [ ] **Step 2: 记录构造与保存重构**

把现有 `const saveRecord = () => { ... };` 整段替换为：

```ts
  const buildRecord = (
    sourceItems: ItemResult[],
    remarkText: string,
    mode: "manual" | "unattended",
  ): SelfTestRecord => {
    const ts = Date.now();
    const sum = summarizeItems(sourceItems);
    return {
      id: `${ts}-${Math.random().toString(36).slice(2, 8)}`,
      ts,
      mac: info.mac,
      version: info.version,
      ip: info.ip,
      speed,
      demo: demoParams,
      items: sourceItems,
      summary: {
        pass: sum.pass,
        fail: sum.fail,
        skip: sum.skip,
        unconfirmed: sum.unconfirmed,
      },
      remark: remarkText,
      mode,
    };
  };

  const saveRecord = () => {
    if (items.length === 0) return;
    const record = buildRecord(items, remark.trim(), runMode);
    setHistory(appendRecord(window.localStorage, record));
    setSaved(true);
  };
```

- [ ] **Step 3: 手动 `start()` 记录模式**

在 `start()` 开头 `setSaved(false);` 下一行加：

```ts
    setAutoSaved(false);
    setRunMode("manual");
```

`retest()` 开头同样在 `setSaved(false);` 下一行加：

```ts
    setAutoSaved(false);
```

- [ ] **Step 4: 新增 `startUnattended()`**

在 `start()` 之后、`retest()` 之前插入：

```ts
  const startUnattended = async () => {
    if (!checked || running) return;
    abortRef.current = false;
    setPrecheckError("");
    setSaved(false);
    setAutoSaved(false);
    setRemark("");
    setRunMode("unattended");

    const outcome = await runSelfTest({
      groups: [...UNATTENDED_GROUPS],
      speed,
      demoParams,
      abortOnFailure: false,
      unattended: true,
      client,
      cameraFrame,
      confirm: askConfirm,
      notice: askNotice,
      isAborted: () => abortRef.current,
      onUpdate: (next, state) => {
        setItems(next);
        setRunState(state);
      },
      onArmSnapshot: setArmSnapshot,
      onCamera: on => {
        if (!on) setCameraSrc("");
      },
    });

    setItems(outcome.items);
    setPrecheckError(outcome.precheckError ?? "");
    setRunState(outcome.state);

    const remarkText = outcome.precheckError
      ? "无人值守（预检失败）"
      : outcome.state === "aborted" ? "无人值守（中途中止）" : "无人值守";
    setRemark(remarkText);
    setHistory(appendRecord(
      window.localStorage,
      buildRecord(outcome.items, remarkText, "unattended"),
    ));
    setSaved(true);
    setAutoSaved(true);
  };
```

- [ ] **Step 5: 更新流程文案与选项说明**

`FLOW_ITEMS` 整段替换为：

```ts
const FLOW_ITEMS = [
  {icon: "🚗", title: "动作", desc: "前进/后退 10cm 与 2s，左右转向 30/60/90/180° 与 2s，抓取/放下"},
  {icon: "📷", title: "摄像头", desc: "打开摄像头、行内预览画面、关闭并确认"},
  {icon: "🎾", title: "自动抓网球", desc: "提示放球后启动视觉抓取脚本；全自动模式不提示、done 即通过"},
  {icon: "🦾", title: "舵机（全自动固定包含）", desc: "底座 / 腕部 / 夹爪逐个大幅转动，最后恢复原始参数"},
  {icon: "📶", title: "WiFi（全自动固定包含）", desc: "扫描并连接 Tsinghua-Dongsheng，不恢复原网络"},
];
```

`heroSubtitle` 的 idle 分支改为：

```ts
    runState === "idle" ? "勾选安全确认后选择「开始测试」（逐项人工确认）或「全自动测试」（五组跑完、自动保存）"
```

在「动作失败后继续」这个 `OptionRow` 之后、`{enableWifi && (...)}` 之前插入：

```tsx
          <div style={{
            marginTop: scalePx(2),
            fontSize: scalePx(9),
            lineHeight: 1.5,
            color: "var(--color-text-dim)",
          }}>
            以上开关只影响「开始测试」；「全自动测试」固定包含全部 5 组、失败不中断。
          </div>
```

- [ ] **Step 6: 插入全自动提示卡与新按钮**

在 `precheckError` 卡片之后、「主按钮」注释之前插入：

```tsx
        {!running && (
          <Card marginBottom={10} style={{background: "rgba(245,158,11,0.08)", border: "1px solid rgba(245,158,11,0.25)"}}>
            <div style={{fontSize: scalePx(13), fontWeight: 800, color: "#b45309", marginBottom: scalePx(3)}}>
              全自动测试（无人值守）
            </div>
            <div style={{fontSize: scalePx(10), lineHeight: 1.7, color: "#b45309"}}>
              <div>按 动作 → 摄像头 → 抓球 → 舵机 → WiFi 一次跑完，中途无确认，失败不中断，结束自动保存历史。</div>
              <div>开始前：网球放在车前 30–60cm；车前后留 ≥1m；机械臂周围清空。</div>
              <div>只按接口成功判定，不检查转向方向 / 距离 / 夹持效果，结果标「无人值守」。</div>
              <div>WiFi 组会切到 Tsinghua-Dongsheng 且不恢复；请通过本车热点 192.168.4.1 打开本页。</div>
            </div>
          </Card>
        )}
```

把原「主按钮」整段（`<div style={{display: "flex", gap: scalePx(8), marginBottom: scalePx(10)}}>` … 到 `</div>`）替换为：

```tsx
        <div style={{display: "flex", gap: scalePx(8), marginBottom: scalePx(10)}}>
          {running ? (
            <>
              <button
                disabled
                style={{
                  flex: 1,
                  padding: `${scalePx(13)} 0`,
                  border: "none",
                  borderRadius: scalePx(13),
                  background: "linear-gradient(135deg, #0a84ff, #5e5ce6)",
                  color: "#fff",
                  fontSize: scalePx(15),
                  fontWeight: 800,
                  cursor: "not-allowed",
                  opacity: 0.75,
                }}
              >
                {runState === "precheck"
                  ? "预检中…"
                  : runMode === "unattended" ? "全自动进行中…" : "测试进行中…"}
              </button>
              <button
                onClick={abort}
                style={{
                  padding: `0 ${scalePx(18)}`,
                  border: "none",
                  borderRadius: scalePx(13),
                  background: "var(--color-danger)",
                  color: "#fff",
                  fontSize: scalePx(14),
                  fontWeight: 700,
                  cursor: "pointer",
                }}
              >
                中止
              </button>
            </>
          ) : (
            <>
              <button
                onClick={start}
                disabled={!checked}
                style={{
                  flex: 1,
                  padding: `${scalePx(13)} 0`,
                  border: "none",
                  borderRadius: scalePx(13),
                  background: checked
                    ? "linear-gradient(135deg, #0a84ff, #5e5ce6)"
                    : "#c7c7cc",
                  color: "#fff",
                  fontSize: scalePx(15),
                  fontWeight: 800,
                  cursor: checked ? "pointer" : "not-allowed",
                  boxShadow: checked ? "0 4px 16px rgba(10,132,255,0.35)" : "none",
                  transition: "background 0.2s, box-shadow 0.2s",
                }}
              >
                {runState === "idle" ? "开始测试" : "重新测试"}
              </button>
              <button
                onClick={startUnattended}
                disabled={!checked}
                style={{
                  flex: 1,
                  padding: `${scalePx(13)} 0`,
                  border: "none",
                  borderRadius: scalePx(13),
                  background: checked
                    ? "linear-gradient(135deg, #30d158, #0a84ff)"
                    : "#c7c7cc",
                  color: "#fff",
                  fontSize: scalePx(15),
                  fontWeight: 800,
                  cursor: checked ? "pointer" : "not-allowed",
                  boxShadow: checked ? "0 4px 16px rgba(48,209,88,0.30)" : "none",
                  transition: "background 0.2s, box-shadow 0.2s",
                }}
              >
                全自动测试
              </button>
            </>
          )}
        </div>
```

- [ ] **Step 7: HistoryPanel 传入自动保存标记**

页面里的 `<HistoryPanel ... />` 加一个属性：

```tsx
              autoSaved={autoSaved}
```

- [ ] **Step 8: HistoryPanel 显示徽标与文案**

`frontend/src/selftest/components/HistoryPanel.tsx`：

接口加属性：

```ts
  saved: boolean;
  /** 本次记录是全自动结束后自动写入的 */
  autoSaved?: boolean;
```

解构加默认值：

```ts
const HistoryPanel = ({
  records, remark, saved, autoSaved = false, canSave, onChangeRemark, onSave, onExport, onClear,
}: HistoryPanelProps) => {
```

保存按钮文案改为：

```tsx
        {saved ? (autoSaved ? "✓ 已自动保存（无人值守）" : "✓ 已保存") : "保存本次记录"}
```

记录行首行改为：

```tsx
          <div style={{display: "flex", alignItems: "center", gap: scalePx(6)}}>
            <span style={{flex: 1, fontSize: scalePx(12), color: "var(--color-text-muted)"}}>{formatTime(record.ts)}</span>
            {record.mode === "unattended" && (
              <span style={{
                fontSize: scalePx(9),
                fontWeight: 700,
                color: "#0a84ff",
                background: "rgba(10,132,255,0.10)",
                borderRadius: "var(--radius-full)",
                padding: `${scalePx(1)} ${scalePx(6)}`,
              }}>全自动</span>
            )}
            <span style={{fontSize: scalePx(11), color: "var(--color-text-dim)"}}>
              通过 {record.summary.pass} / 失败 {record.summary.fail}
              {record.summary.skip > 0 ? ` / 跳过 ${record.summary.skip}` : ""}
              {record.summary.unconfirmed > 0 ? ` / 未确认 ${record.summary.unconfirmed}` : ""}
            </span>
          </div>
```

- [ ] **Step 9: 编译与回归**

Run（`AKA-00/frontend`）：

```bash
npx tsc -b && npm run test:selftest
```

Expected: tsc 退出码 0；测试 `fail 0`。

- [ ] **Step 10: 提交**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00"
git add frontend/src/pages/SelfTestPage.tsx frontend/src/selftest/components/HistoryPanel.tsx
git commit -m "feat(selftest): 页面新增全自动测试入口与无人值守自动保存"
```

---

### Task 5: 全量测试与 noscreen 构建

**Files:**
- 无源码改动；产物：`static/index.html`、`static/assets/index.{js,js.br,js.gz,css}`（`static/` 已被 gitignore，不提交）

- [ ] **Step 1: 全量单测**

Run：

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00/frontend"
npm run test:selftest
```

Expected: 全部 PASS，`fail 0`，`canceled 0`。

- [ ] **Step 2: 类型检查**

```bash
npx tsc -b
```

Expected: 退出码 0，无输出。

- [ ] **Step 3: 构建 noscreen 前端（产物直接写 `AKA-00/static`）**

```bash
npm run build:noscreen
```

Expected: 依次完成 `tsc -b`、`vite build`（输出 `built in …ms`）、`sri-retry`、`gzip-assets`，退出码 0。

- [ ] **Step 4: 核对产物**

```bash
ls -l ../static/index.html ../static/assets/index.js ../static/assets/index.js.br ../static/assets/index.js.gz ../static/assets/index.css
grep -c "SRI-RETRY" ../static/index.html
```

Expected: 5 个文件时间为刚构建；`grep` 输出 `1`。

- [ ] **Step 5: 提交**

本任务无 git 改动（`static/` 被忽略）。若 `git status` 显示计划外文件被改动，停止并排查，不要顺手提交。

---

### Task 6: 部署到小车并真机验收（需操作者确认）

**Files:**
- 上传：`static/index.html`、`static/assets/index.js`、`static/assets/index.js.br`、`static/assets/index.js.gz`、`static/assets/index.css` → 车机 `/root/AKA-00/static/`

- [ ] **Step 1: 与操作者确认目标车与网络**

询问当前要部署的小车 IP，并确认本机可访问。验证：

```bash
curl -s -o /dev/null -w "%{http_code}\n" "http://<车机IP>/settings"
```

Expected: `200`。若不通，先排查网络（不要盲传）。

- [ ] **Step 2: 上传 5 个文件并逐文件校验**

Run（`MSYS_NO_PATHCONV=1` 防止 Git Bash 改写远端路径）：

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车实验/car-ai-senses/.debug"
MSYS_NO_PATHCONV=1 CAR_HOST=<车机IP> python upload_verify.py \
  "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00/static" /root/AKA-00/static \
  index.html assets/index.js assets/index.js.br assets/index.js.gz assets/index.css
```

Expected：5 行 `ok`，结尾 `== ok 5 bad 0`。

- [ ] **Step 3: 清理过期入口边车**

```bash
MSYS_NO_PATHCONV=1 CAR_HOST=<车机IP> python remote_exec.py \
  "rm -f /root/AKA-00/static/index.html.br /root/AKA-00/static/index.html.gz; ls /root/AKA-00/static/assets/index.js* /root/AKA-00/static/index.html"
```

Expected：只列出 `index.js`、`index.js.br`、`index.js.gz`、`index.html`。

- [ ] **Step 4: 页面验收（人工）**

用手机/浏览器打开 `http://<车机IP>/self-test`，逐条核对：

1. 出现「开始测试」与「全自动测试」两个按钮；未勾选安全确认时两者都不可点。
2. 「全自动测试」上方能看到四条前置提示（含 WiFi/192.168.4.1 提示）。
3. 勾选安全确认后点「全自动测试」：全程无任何弹窗；顺序为动作 → 摄像头 → 抓球 → 舵机 → WiFi；某项失败后其余动作项仍继续（不再出现「前一项失败，已跳过」）。
4. 结束后自动出现历史记录，带「全自动」徽标、备注「无人值守」；保存按钮显示「✓ 已自动保存（无人值守）」。
5. 点「重新测试」（手动）→ 仍按原流程弹确认、失败默认连锁跳过、手动保存可用。
6. 检查舵机结束后角度已恢复（可用行内 ↻ 重测舵机项或看设备状态）。
7. 若中途点「中止」，历史备注为「无人值守（中途中止）」。

- [ ] **Step 5: 记录开发日志**

```bash
node ~/.claude/session-log/session-log.mjs log "AKA-00 /self-test 全自动模式上线：runner unattended 开关（无确认/不连锁跳过/接口判定）、WiFi 组后置、双按钮入口、自动保存历史带 mode 徽标；npm run test:selftest 全过，noscreen 构建并部署 <车机IP>"
```

---

## 自检结果

- **规格覆盖：** §3 页面（Task 4）、§4 runner 规则（Task 2）、§4.1 顺序（Task 3）、§5 记录与自动保存（Tasks 1/4）、§7 测试（Tasks 1-3/5）、§8 构建部署（Tasks 5-6）、§9 验收（Task 6 Step 4）。无遗漏。
- **占位符：** 仅 Task 6 的 `<车机IP>` 为部署时由操作者确认的参数（执行前必须询问，非计划缺口）。
- **类型一致性：** `unattended`、`UNATTENDED_GROUPS`、`mode`、`runMode`、`autoSaved`、`buildRecord`、`startUnattended` 在测试、实现与页面中名称一致。
