# 小车自动测试页面（/self-test）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在控制台设置里新增一个自动测试页面，按顺序执行动作、摄像头、自动抓网球（可选 WiFi、舵机），人工确认视觉结果，历史与备注保存在浏览器本地并可导出。

**Architecture:** 纯前端实现，不改固件。纯逻辑抽到 `src/selftest/`（URL 构造、角度调整、历史存储、串行 runner），用 Node 内置测试运行器覆盖；React 页面只负责展示、确认弹窗和摄像头画面。

**Tech Stack:** React 19 + React Router 7 + Vite 8 + TypeScript 5.9；测试用 `node --experimental-strip-types --test`（Node v22.16 已具备）。

**参考规格：** `docs/superpowers/specs/2026-11-13-selftest-page-design.md`

**关于提交：** 仓库当前已有一批未提交改动，本计划**不自动执行 `git commit`**。每个 Task 完成后由操作者自行决定是否提交。

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `frontend/src/selftest/core.ts` | 纯逻辑：类型、URL 构造、结果判定、角度调整、汇总、测试项常量 |
| `frontend/src/selftest/history.ts` | localStorage 读写、导出文件名（存储接口可注入，便于测试） |
| `frontend/src/selftest/client.ts` | 浏览器侧 HTTP 适配层（fetch + 超时 + 解析），实现 `Client` 接口 |
| `frontend/src/selftest/runner.ts` | 串行执行状态机：预检、动作、摄像头、自动抓球、WiFi、舵机、清理 |
| `frontend/src/selftest/components/ItemList.tsx` | 测试项列表展示（状态、子步骤、耗时、错误） |
| `frontend/src/selftest/components/HistoryPanel.tsx` | 备注、保存、历史列表、导出/清空 |
| `frontend/src/pages/SelfTestPage.tsx` | 页面组装：设备卡、安全确认、选项、控制栏、确认弹窗、摄像头画面 |
| `frontend/tests/core.test.ts` | core 纯函数测试 |
| `frontend/tests/history.test.ts` | 历史存储测试 |
| `frontend/tests/runner.test.ts` | runner 顺序/判据/清理测试（fake client） |
| `frontend/package.json` | 新增 `test:selftest` 脚本 |
| `frontend/src/App.tsx` | 新增 `/self-test` 路由 |
| `frontend/src/pages/SettingsPage.tsx` | 新增“自动测试”入口 |

---

## Task 1: 测试脚本与 core 的 URL/结果判定

**Files:**
- Create: `frontend/tests/core.test.ts`
- Create: `frontend/src/selftest/core.ts`
- Modify: `frontend/package.json:6-12`

- [ ] **Step 1: 写失败测试**

创建 `frontend/tests/core.test.ts`：

```ts
import test from "node:test";
import assert from "node:assert/strict";
import {
  MOTION_ROWS,
  adjustArmAngles,
  buildControlUrl,
  classifyControl,
  sameAngles,
  summarizeItems,
  type ItemResult,
} from "../src/selftest/core.ts";

test("buildControlUrl: distance/angle 优先于 time，speed 保留", () => {
  assert.equal(buildControlUrl("up", {distance: 10, speed: 25}), "/api/control?action=up&speed=25&distance=10");
  assert.equal(buildControlUrl("left", {angle: 90, speed: 25}), "/api/control?action=left&speed=25&angle=90");
  assert.equal(buildControlUrl("up", {time: 2000, speed: 25}), "/api/control?action=up&speed=25&time=2000");
  assert.equal(buildControlUrl("stop"), "/api/control?action=stop");
});

test("classifyControl: 只有 2xx + completed 才算成功", () => {
  assert.deepEqual(classifyControl(200, {completed: true}), {ok: true, completed: true});
  assert.deepEqual(classifyControl(200, {completed: false, error: "夹爪正忙"}), {ok: false, completed: false, error: "夹爪正忙"});
  assert.equal(classifyControl(500, {error: "boom"}).ok, false);
  assert.equal(classifyControl(200, null).error, "HTTP 200");
});

test("MOTION_ROWS: 7 对动作 + 抓取，参数符合规格", () => {
  assert.equal(MOTION_ROWS.length, 8);
  const ids = MOTION_ROWS.map(row => row.id);
  assert.deepEqual(ids, ["move_cm", "move_time", "turn_30", "turn_60", "turn_90", "turn_180", "turn_time", "grab"]);
  const angles = MOTION_ROWS
    .filter(row => row.kind === "pair" && row.id.startsWith("turn_") && row.id !== "turn_time")
    .map(row => row.kind === "pair" ? row.first.opts.angle : 0);
  assert.deepEqual(angles, [30, 60, 90, 180]);
});

test("adjustArmAngles: 优先 gripper_open，按驱动步进并夹紧", () => {
  const zp = adjustArmAngles("zp10s", {gripper_open: 150, gripper_close: 90});
  assert.equal(zp.key, "gripper_open");
  assert.equal(zp.from, 150);
  assert.equal(zp.to, 160);
  assert.equal(zp.next.gripper_open, 160);

  const edge = adjustArmAngles("zp10s", {gripper_open: 270});
  assert.equal(edge.to, 260);

  const sts = adjustArmAngles("sts3215", {gripper_open: 1000});
  assert.equal(sts.to, 1100);

  const nested = adjustArmAngles("zp10s", {grab_position: {servo0: 245, servo1: 180}});
  assert.equal(nested.key, "grab_position.servo0");
  assert.equal(nested.to, 255);
});

test("sameAngles: 忽略对象键顺序", () => {
  assert.equal(sameAngles({a: 1, b: {c: 2, d: 3}}, {b: {d: 3, c: 2}, a: 1}), true);
  assert.equal(sameAngles({a: 1}, {a: 2}), false);
});

test("summarizeItems: 统计 pass/fail/skip/unconfirmed", () => {
  const item = (status: ItemResult["status"]): ItemResult => ({
    id: status, name: status, group: "motion", status,
    auto: null, manual: null, steps: [], elapsedMs: 0,
  });
  const summary = summarizeItems([item("pass"), item("fail"), item("skip"), item("unconfirmed")]);
  assert.deepEqual(
    {pass: summary.pass, fail: summary.fail, skip: summary.skip, unconfirmed: summary.unconfirmed},
    {pass: 1, fail: 1, skip: 1, unconfirmed: 1},
  );
  assert.equal(summary.ok, false);
  assert.equal(summarizeItems([item("pass"), item("skip")]).ok, true);
});
```

- [ ] **Step 2: 加测试脚本**

修改 `frontend/package.json` 的 `scripts`，在 `"lint"` 前加一行：

```json
    "test:selftest": "node --experimental-strip-types --test tests/core.test.ts tests/history.test.ts tests/runner.test.ts",
```

- [ ] **Step 3: 运行测试确认失败**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00/frontend"
npm run test:selftest
```

预期：`ERR_MODULE_NOT_FOUND`，找不到 `src/selftest/core.ts`。

- [ ] **Step 4: 创建 `core.ts` 的类型与 URL/判定部分**

创建 `frontend/src/selftest/core.ts`：

```ts
// 自动测试的纯逻辑：不依赖 DOM / fetch，可用 node --test 直接跑。
// 注意：Node 的 --experimental-strip-types 只支持可擦除语法（无 enum / namespace）。

export type Group = "motion" | "camera" | "demo" | "wifi" | "arm";
export type RunState = "idle" | "precheck" | "running" | "finished" | "aborted";
export type ItemStatus = "pending" | "running" | "confirm" | "pass" | "fail" | "skip" | "unconfirmed";

export interface ControlOptions {
  distance?: number;
  angle?: number;
  time?: number;
  speed?: number;
}

export interface StepResult {
  name: string;
  ok: boolean;
  elapsedMs: number;
  error?: string;
  detail?: string;
}

export interface ItemResult {
  id: string;
  name: string;
  group: Group;
  status: ItemStatus;
  auto: "pass" | "fail" | null;
  manual: "pass" | "fail" | null;
  steps: StepResult[];
  elapsedMs: number;
  error?: string;
}

export interface SelfTestSummary {
  pass: number;
  fail: number;
  skip: number;
  unconfirmed: number;
}

export interface HumanSummary extends SelfTestSummary {
  total: number;
  ok: boolean;
}

export interface SelfTestRecord {
  id: string;
  ts: number;
  mac: string;
  version: string;
  ip: string;
  speed: number;
  items: ItemResult[];
  summary: SelfTestSummary;
  remark: string;
}

export function buildControlUrl(action: string, opts: ControlOptions = {}): string {
  const params = new URLSearchParams({action});
  if (opts.speed !== undefined) params.set("speed", String(opts.speed));
  if (opts.distance !== undefined) params.set("distance", String(opts.distance));
  else if (opts.angle !== undefined) params.set("angle", String(opts.angle));
  else if (opts.time !== undefined && opts.time > 0) params.set("time", String(opts.time));
  return `/api/control?${params.toString()}`;
}

export interface ControlVerdict {
  ok: boolean;
  completed: boolean;
  error?: string;
}

export function classifyControl(status: number, body: unknown): ControlVerdict {
  const data = body && typeof body === "object" ? body as Record<string, unknown> : {};
  const completed = data.completed === true;
  if (status >= 200 && status < 300 && completed) return {ok: true, completed: true};
  const error = typeof data.error === "string" && data.error ? data.error : `HTTP ${status}`;
  return {ok: false, completed, error};
}
```

- [ ] **Step 5: 运行测试，确认 URL/判定用例通过，其余仍失败**

```bash
npm run test:selftest
```

预期：前两个用例通过；`MOTION_ROWS`/`adjustArmAngles` 等因未导出而失败（`SyntaxError: The requested module ... does not provide an export named`）。

---

## Task 2: core 的角度调整、比较、汇总与测试项常量

**Files:**
- Modify: `frontend/src/selftest/core.ts`

- [ ] **Step 1: 追加角度与汇总实现**

在 `core.ts` 末尾追加：

```ts
export function canonicalJson(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(canonicalJson).join(",")}]`;
  if (value && typeof value === "object") {
    const obj = value as Record<string, unknown>;
    return `{${Object.keys(obj).sort().map(k => `${JSON.stringify(k)}:${canonicalJson(obj[k])}`).join(",")}}`;
  }
  return JSON.stringify(value);
}

export function sameAngles(a: unknown, b: unknown): boolean {
  return canonicalJson(a) === canonicalJson(b);
}

interface NumberLeaf {
  path: string[];
  value: number;
}

function firstNumberLeaf(node: unknown, prefix: string[] = []): NumberLeaf | null {
  if (typeof node === "number" && Number.isFinite(node)) return {path: prefix, value: node};
  if (node && typeof node === "object") {
    for (const key of Object.keys(node as Record<string, unknown>)) {
      const found = firstNumberLeaf((node as Record<string, unknown>)[key], [...prefix, key]);
      if (found) return found;
    }
  }
  return null;
}

export interface ArmAdjust {
  key: string;
  from: number;
  to: number;
  next: Record<string, unknown>;
}

export function adjustArmAngles(driver: string, angles: Record<string, unknown>): ArmAdjust {
  const range = driver === "sts3215"
    ? {min: 0, max: 4095, step: 100}
    : {min: 0, max: 270, step: 10};
  const target: NumberLeaf | null = typeof angles.gripper_open === "number"
    ? {path: ["gripper_open"], value: angles.gripper_open}
    : firstNumberLeaf(angles);
  if (!target) throw new Error("角度配置里没有可调整的数值");

  let to = Math.min(range.max, target.value + range.step);
  if (to === target.value) to = Math.max(range.min, target.value - range.step);
  if (to === target.value) throw new Error("角度已在边界，无法调整");

  const next = JSON.parse(JSON.stringify(angles)) as Record<string, unknown>;
  let cursor: Record<string, unknown> = next;
  for (let i = 0; i < target.path.length - 1; i += 1) {
    cursor = cursor[target.path[i]] as Record<string, unknown>;
  }
  cursor[target.path[target.path.length - 1]] = to;
  return {key: target.path.join("."), from: target.value, to, next};
}

export function summarizeItems(items: ItemResult[]): HumanSummary {
  const summary: HumanSummary = {pass: 0, fail: 0, skip: 0, unconfirmed: 0, total: items.length, ok: false};
  for (const item of items) {
    if (item.status === "pass") summary.pass += 1;
    else if (item.status === "fail") summary.fail += 1;
    else if (item.status === "skip") summary.skip += 1;
    else summary.unconfirmed += 1;
  }
  summary.ok = summary.fail === 0 && summary.unconfirmed === 0 && summary.pass > 0;
  return summary;
}
```

- [ ] **Step 2: 追加测试项常量**

继续在 `core.ts` 末尾追加：

```ts
export interface Cmd {
  name: string;
  action: string;
  opts: ControlOptions;
  timeoutMs: number;
}

export type MotionRow =
  | {kind: "pair"; id: string; label: string; first: Cmd; second: Cmd}
  | {kind: "grab"; id: "grab"; label: string};

const pair = (id: string, label: string, first: Cmd, second: Cmd): MotionRow =>
  ({kind: "pair", id, label, first, second});

export const MOTION_ROWS: MotionRow[] = [
  pair("move_cm", "前进 / 后退 10cm",
    {name: "前进 10cm", action: "up", opts: {distance: 10}, timeoutMs: 35000},
    {name: "后退 10cm", action: "down", opts: {distance: 10}, timeoutMs: 35000}),
  pair("move_time", "前进 / 后退 2s",
    {name: "前进 2s", action: "up", opts: {time: 2000}, timeoutMs: 7000},
    {name: "后退 2s", action: "down", opts: {time: 2000}, timeoutMs: 7000}),
  pair("turn_30", "左转 / 右转 30°",
    {name: "左转 30°", action: "left", opts: {angle: 30}, timeoutMs: 35000},
    {name: "右转 30°", action: "right", opts: {angle: 30}, timeoutMs: 35000}),
  pair("turn_60", "左转 / 右转 60°",
    {name: "左转 60°", action: "left", opts: {angle: 60}, timeoutMs: 35000},
    {name: "右转 60°", action: "right", opts: {angle: 60}, timeoutMs: 35000}),
  pair("turn_90", "左转 / 右转 90°",
    {name: "左转 90°", action: "left", opts: {angle: 90}, timeoutMs: 35000},
    {name: "右转 90°", action: "right", opts: {angle: 90}, timeoutMs: 35000}),
  pair("turn_180", "左转 / 右转 180°",
    {name: "左转 180°", action: "left", opts: {angle: 180}, timeoutMs: 35000},
    {name: "右转 180°", action: "right", opts: {angle: 180}, timeoutMs: 35000}),
  pair("turn_time", "左转 / 右转 2s",
    {name: "左转 2s", action: "left", opts: {time: 2000}, timeoutMs: 7000},
    {name: "右转 2s", action: "right", opts: {time: 2000}, timeoutMs: 7000}),
  {kind: "grab", id: "grab", label: "抓取 → 放下"},
];

export const WIFI_SSID = "Tsinghua-Dongsheng";
export const DEMO_TIMEOUT_MS = 90000;
export const DEMO_PARAMS = {
  action: "grab",
  model: "tennis",
  target_size: 280,
  speed: 20,
  turn_speed: 18,
  mode: "once",
} as const;
```

- [ ] **Step 3: 运行 core 测试**

```bash
npm run test:selftest
```

预期：`core.test.ts` 的 6 个用例全部通过；`history.test.ts` / `runner.test.ts` 尚不存在，Node 报找不到文件退出（后续 Task 补齐）。

---

## Task 3: 历史存储模块

**Files:**
- Create: `frontend/tests/history.test.ts`
- Create: `frontend/src/selftest/history.ts`

- [ ] **Step 1: 写失败测试**

创建 `frontend/tests/history.test.ts`：

```ts
import test from "node:test";
import assert from "node:assert/strict";
import {
  HISTORY_KEY,
  HISTORY_LIMIT,
  appendRecord,
  clearHistory,
  exportFilename,
  loadHistory,
} from "../src/selftest/history.ts";
import type { StorageLike } from "../src/selftest/history.ts";
import type { SelfTestRecord } from "../src/selftest/core.ts";

function memoryStorage(): StorageLike {
  const data = new Map<string, string>();
  return {
    getItem: key => data.get(key) ?? null,
    setItem: (key, value) => { data.set(key, value); },
    removeItem: key => { data.delete(key); },
  };
}

function record(ts: number): SelfTestRecord {
  return {
    id: `r${ts}`, ts, mac: "AA:BB:CC:DD:EE:FF", version: "v1",
    ip: "1.2.3.4", speed: 25, items: [],
    summary: {pass: 0, fail: 0, skip: 0, unconfirmed: 0}, remark: "",
  };
}

test("appendRecord: 新记录在前，最多保留 HISTORY_LIMIT 条", () => {
  const storage = memoryStorage();
  for (let i = 1; i <= HISTORY_LIMIT + 5; i += 1) appendRecord(storage, record(i));
  const list = loadHistory(storage);
  assert.equal(list.length, HISTORY_LIMIT);
  assert.equal(list[0].ts, HISTORY_LIMIT + 5);
  assert.equal(list[HISTORY_LIMIT - 1].ts, 6);
});

test("loadHistory: 非法 JSON 返回空数组", () => {
  const storage = memoryStorage();
  storage.setItem(HISTORY_KEY, "{oops");
  assert.deepEqual(loadHistory(storage), []);
});

test("clearHistory 删除键", () => {
  const storage = memoryStorage();
  appendRecord(storage, record(1));
  assert.equal(loadHistory(storage).length, 1);
  clearHistory(storage);
  assert.equal(loadHistory(storage).length, 0);
});

test("exportFilename: 去掉 MAC 冒号并带时间戳", () => {
  const ts = new Date(2026, 10, 13, 9, 5, 7).getTime();
  assert.equal(exportFilename("AA:BB:CC:DD:EE:FF", ts), "selftest-AA-BB-CC-DD-EE-FF-20261113-090507.json");
});
```

- [ ] **Step 2: 运行确认失败**

```bash
npm run test:selftest
```

预期：`ERR_MODULE_NOT_FOUND` 找不到 `src/selftest/history.ts`。

- [ ] **Step 3: 实现 `history.ts`**

创建 `frontend/src/selftest/history.ts`：

```ts
import type { SelfTestRecord } from "./core.ts";

export const HISTORY_KEY = "aka_selftest_history_v1";
export const HISTORY_LIMIT = 100;

export interface StorageLike {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

export function loadHistory(storage: StorageLike): SelfTestRecord[] {
  try {
    const raw = storage.getItem(HISTORY_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw) as unknown;
    return Array.isArray(parsed) ? parsed as SelfTestRecord[] : [];
  } catch {
    return [];
  }
}

export function appendRecord(storage: StorageLike, record: SelfTestRecord): SelfTestRecord[] {
  const next = [record, ...loadHistory(storage)].slice(0, HISTORY_LIMIT);
  storage.setItem(HISTORY_KEY, JSON.stringify(next));
  return next;
}

export function clearHistory(storage: StorageLike): void {
  storage.removeItem(HISTORY_KEY);
}

export function exportFilename(mac: string, ts: number): string {
  const date = new Date(ts);
  const pad = (n: number) => String(n).padStart(2, "0");
  const stamp = `${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}-`
    + `${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`;
  return `selftest-${mac.replace(/[:.]/g, "-")}-${stamp}.json`;
}
```

- [ ] **Step 4: 运行测试**

```bash
npm run test:selftest
```

预期：`core.test.ts` 6 个 + `history.test.ts` 4 个全部通过；`runner.test.ts` 仍不存在而报错。

---

## Task 4: HTTP 适配层

**Files:**
- Create: `frontend/src/selftest/client.ts`

- [ ] **Step 1: 实现 `client.ts`**

创建 `frontend/src/selftest/client.ts`：

```ts
import { DEMO_PARAMS, buildControlUrl, classifyControl } from "./core.ts";
import type { ControlOptions } from "./core.ts";

export interface ApiResult<T> {
  ok: boolean;
  status: number;
  data: T | null;
  error?: string;
  elapsedMs: number;
}

export async function requestJson<T = unknown>(
  url: string,
  init?: RequestInit,
  timeoutMs = 10000,
): Promise<ApiResult<T>> {
  const controller = new AbortController();
  const timer = window.setTimeout(() => controller.abort(), timeoutMs);
  const started = performance.now();
  try {
    const response = await fetch(url, {...init, signal: controller.signal});
    const text = await response.text();
    let data: unknown = null;
    try {
      data = text ? JSON.parse(text) : null;
    } catch {
      data = text;
    }
    const body = data && typeof data === "object" ? data as Record<string, unknown> : {};
    const error = response.ok
      ? undefined
      : (typeof body.error === "string" ? body.error : `HTTP ${response.status}`);
    return {
      ok: response.ok,
      status: response.status,
      data: data as T | null,
      error,
      elapsedMs: Math.round(performance.now() - started),
    };
  } catch (e) {
    const error = e instanceof Error && e.name === "AbortError" ? `超时 ${timeoutMs}ms` : String(e);
    return {ok: false, status: 0, data: null, error, elapsedMs: Math.round(performance.now() - started)};
  } finally {
    window.clearTimeout(timer);
  }
}

export interface ControlOutcome {
  ok: boolean;
  completed: boolean;
  error?: string;
  elapsedMs: number;
}

export interface SimpleOutcome {
  ok: boolean;
  error?: string;
}

export interface CameraOutcome {
  ok: boolean;
  camera_on: boolean;
  error?: string;
}

export interface DemoStatusOutcome {
  ok: boolean;
  state: string;
  message?: string;
}

export interface WifiNetwork {
  ssid: string;
  signal: number;
  secured: boolean;
}

export interface WifiScanOutcome {
  ok: boolean;
  list: WifiNetwork[];
  error?: string;
}

export interface WifiConnectOutcome {
  ok: boolean;
  ip?: string;
  error?: string;
}

export interface ArmReadOutcome {
  ok: boolean;
  driver: string;
  angles: Record<string, unknown>;
  error?: string;
}

export interface WifiStatusOutcome {
  ok: boolean;
  ssid: string | null;
  ip: string;
}

export interface Client {
  control(action: string, opts: ControlOptions & {timeoutMs: number}): Promise<ControlOutcome>;
  stop(): Promise<void>;
  cameraOpen(): Promise<CameraOutcome>;
  cameraClose(): Promise<CameraOutcome>;
  cameraStatus(): Promise<CameraOutcome>;
  demoStart(): Promise<SimpleOutcome>;
  demoStatus(): Promise<DemoStatusOutcome>;
  demoStop(): Promise<void>;
  demoHasTennis(): Promise<boolean>;
  wifiScan(): Promise<WifiScanOutcome>;
  wifiConnect(ssid: string, password: string): Promise<WifiConnectOutcome>;
  wifiStatus(): Promise<WifiStatusOutcome>;
  armRead(): Promise<ArmReadOutcome>;
  armWrite(driver: string, angles: Record<string, unknown>): Promise<SimpleOutcome>;
  motorConnected(): Promise<boolean>;
  systemInfo(): Promise<{mac: string; ip: string}>;
  version(): Promise<string>;
  sleep(ms: number): Promise<void>;
}

export function createClient(): Client {
  return {
    control: async (action, opts) => {
      const result = await requestJson(buildControlUrl(action, opts), undefined, opts.timeoutMs);
      return {...classifyControl(result.status, result.data), elapsedMs: result.elapsedMs};
    },

    stop: async () => {
      await requestJson("/api/control?action=stop", undefined, 5000);
    },

    cameraOpen: async () => {
      const result = await requestJson<{camera_on?: boolean}>("/api/camera/open", {method: "POST"}, 8000);
      return {
        ok: result.ok && result.data?.camera_on === true,
        camera_on: result.data?.camera_on === true,
        error: result.error,
      };
    },

    cameraClose: async () => {
      const result = await requestJson<{camera_on?: boolean}>("/api/camera/close", {method: "POST"}, 8000);
      return {ok: result.ok, camera_on: result.data?.camera_on === true, error: result.error};
    },

    cameraStatus: async () => {
      const result = await requestJson<{camera_on?: boolean}>("/api/camera/status", undefined, 5000);
      return {ok: result.ok, camera_on: result.data?.camera_on === true, error: result.error};
    },

    demoStart: async () => {
      const result = await requestJson("/api/demo/init", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({...DEMO_PARAMS, wait: false}),
      }, 15000);
      return {ok: result.ok, error: result.error};
    },

    demoStatus: async () => {
      const result = await requestJson<{state?: string; message?: string}>("/api/demo/status", undefined, 5000);
      return {ok: result.ok, state: String(result.data?.state ?? ""), message: result.data?.message};
    },

    demoStop: async () => {
      await requestJson("/api/demo/stop", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: "{}",
      }, 8000);
    },

    demoHasTennis: async () => {
      const result = await requestJson<{models?: unknown}>("/api/demo/list", undefined, 8000);
      const raw = result.data?.models;
      const models: unknown[] = Array.isArray(raw) ? raw : [];
      return models.some(model => {
        if (typeof model === "string") return model === "tennis";
        if (model && typeof model === "object") return (model as {name?: unknown}).name === "tennis";
        return false;
      });
    },

    wifiScan: async () => {
      const result = await requestJson<{list?: unknown}>("/api/wifi/scan", undefined, 10000);
      const raw = result.data?.list;
      const list: unknown[] = Array.isArray(raw) ? raw : [];
      return {
        ok: result.ok,
        list: list.map(net => {
          const item = net as Record<string, unknown>;
          return {
            ssid: String(item.ssid ?? ""),
            signal: Number(item.signal ?? 0),
            secured: item.secured === true,
          };
        }).filter(net => net.ssid),
        error: result.error,
      };
    },

    wifiConnect: async (ssid, password) => {
      const result = await requestJson<{ip?: string}>("/api/wifi/connect", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({ssid, password}),
      }, 25000);
      return {
        ok: result.ok,
        ip: typeof result.data?.ip === "string" ? result.data.ip : undefined,
        error: result.error,
      };
    },

    wifiStatus: async () => {
      const [status, ip] = await Promise.all([
        requestJson<{ssid?: string | null}>("/api/wifi/status", undefined, 8000),
        requestJson<{ip?: string}>("/api/wifi/ip", undefined, 8000),
      ]);
      return {
        ok: status.ok,
        ssid: typeof status.data?.ssid === "string" ? status.data.ssid : null,
        ip: String(ip.data?.ip ?? ""),
      };
    },

    armRead: async () => {
      const result = await requestJson<{driver?: string; angles?: Record<string, unknown>}>(
        "/api/arm/angles", undefined, 5000,
      );
      return {
        ok: result.ok && !!result.data?.angles,
        driver: String(result.data?.driver ?? "zp10s"),
        angles: result.data?.angles ?? {},
        error: result.error,
      };
    },

    armWrite: async (driver, angles) => {
      const result = await requestJson("/api/arm/angles", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({driver, angles}),
      }, 8000);
      return {ok: result.ok, error: result.error};
    },

    motorConnected: async () => {
      const result = await requestJson<{motor?: {connected?: boolean}}>("/api/motor/status", undefined, 5000);
      return result.ok && result.data?.motor?.connected === true;
    },

    systemInfo: async () => {
      const result = await requestJson<{mac?: string; ip?: string}>("/api/system/info", undefined, 5000);
      return {mac: String(result.data?.mac ?? ""), ip: String(result.data?.ip ?? "")};
    },

    version: async () => {
      const result = await requestJson<{version?: string}>("/api/ota/version", undefined, 5000);
      return String(result.data?.version ?? "");
    },

    sleep: (ms) => new Promise<void>(resolve => window.setTimeout(resolve, ms)),
  };
}
```

- [ ] **Step 2: 类型检查**

```bash
npx tsc -b
```

预期：无输出、退出码 0。

---

## Task 5: runner 预检与动作组

**Files:**
- Create: `frontend/tests/runner.test.ts`
- Create: `frontend/src/selftest/runner.ts`

- [ ] **Step 1: 写失败测试（预检 + 动作）**

创建 `frontend/tests/runner.test.ts`：

```ts
import test from "node:test";
import assert from "node:assert/strict";
import {runSelfTest} from "../src/selftest/runner.ts";
import type { Client } from "../src/selftest/client.ts";
import type { Group, ItemResult } from "../src/selftest/core.ts";

interface Fake {
  client: Client;
  calls: string[];
}

function fakeClient(): Fake {
  const calls: string[] = [];
  const client: Client = {
    control: async (action, opts) => {
      const suffix = opts.distance !== undefined ? `@${opts.distance}cm`
        : opts.angle !== undefined ? `@${opts.angle}deg`
        : opts.time !== undefined ? `@${opts.time}ms` : "";
      calls.push(`${action}${suffix}`);
      return {ok: true, completed: true, elapsedMs: 1};
    },
    stop: async () => { calls.push("stop"); },
    cameraOpen: async () => { calls.push("camera.open"); return {ok: true, camera_on: true}; },
    cameraClose: async () => { calls.push("camera.close"); return {ok: true, camera_on: false}; },
    cameraStatus: async () => ({ok: true, camera_on: false}),
    demoStart: async () => { calls.push("demo.start"); return {ok: true}; },
    demoStatus: async () => ({ok: true, state: "done"}),
    demoStop: async () => { calls.push("demo.stop"); },
    demoHasTennis: async () => true,
    wifiScan: async () => ({ok: true, list: [{ssid: "Tsinghua-Dongsheng", signal: -40, secured: false}]}),
    wifiConnect: async () => ({ok: true, ip: "192.168.9.9"}),
    wifiStatus: async () => ({ok: true, ssid: "Tsinghua-Dongsheng", ip: "192.168.9.9"}),
    armRead: async () => ({ok: true, driver: "zp10s", angles: {gripper_open: 150, gripper_close: 90}}),
    armWrite: async () => ({ok: true}),
    motorConnected: async () => true,
    systemInfo: async () => ({mac: "AA:BB:CC:DD:EE:FF", ip: "1.2.3.4"}),
    version: async () => "v1",
    sleep: async () => {},
  };
  return {client, calls};
}

async function run(groups: Group[], overrides: Partial<Client> = {}, options: {
  isAborted?: () => boolean;
  confirm?: (id: string) => boolean;
  now?: () => number;
} = {}) {
  const fake = fakeClient();
  Object.assign(fake.client, overrides);
  const outcome = await runSelfTest({
    groups,
    speed: 25,
    abortOnFailure: true,
    client: fake.client,
    cameraFrame: async () => true,
    confirm: async (id) => options.confirm ? options.confirm(id) : true,
    isAborted: options.isAborted ?? (() => false),
    onUpdate: () => {},
    onArmSnapshot: () => {},
    onCamera: () => {},
    now: options.now,
  });
  return {fake, outcome};
}

function motionItems(items: ItemResult[]): ItemResult[] {
  return items.filter(item => item.group === "motion");
}

test("预检：底盘未连接时中止，不发任何控制命令", async () => {
  const {fake, outcome} = await run(["motion"], {motorConnected: async () => false});
  assert.equal(outcome.state, "aborted");
  assert.match(outcome.precheckError ?? "", /底盘未连接/);
  assert.deepEqual(fake.calls.filter(call => !call.startsWith("camera.")), ["stop"]);
  assert.equal(motionItems(outcome.items).every(item => item.status === "fail"), true);
});

test("动作组：7 对 + 抓取按顺序执行，全部通过", async () => {
  const {fake, outcome} = await run(["motion"]);
  assert.equal(outcome.state, "finished");
  assert.deepEqual(motionItems(outcome.items).map(item => item.status), Array(8).fill("pass"));
  assert.deepEqual(fake.calls.filter(call => call.startsWith("camera.") || call === "stop"), ["camera.close", "stop"]);
  assert.deepEqual(fake.calls.filter(call => !call.startsWith("camera.") && call !== "stop"), [
    "up@10cm", "down@10cm",
    "up@2000ms", "down@2000ms",
    "left@30deg", "right@30deg",
    "left@60deg", "right@60deg",
    "left@90deg", "right@90deg",
    "left@180deg", "right@180deg",
    "left@2000ms", "right@2000ms",
    "grab", "release",
  ]);
});

test("动作失败：默认跳过后续动作项，但仍执行清理", async () => {
  const {fake, outcome} = await run(["motion"], {
    control: async (action, opts) => {
      const suffix = opts.distance !== undefined ? `@${opts.distance}cm` : "";
      fake.calls.push(`${action}${suffix}`);
      return fake.calls.length === 1
        ? {ok: false, completed: false, error: "动作失败", elapsedMs: 1}
        : {ok: true, completed: true, elapsedMs: 1};
    },
  });
  const items = motionItems(outcome.items);
  assert.equal(items[0].status, "fail");
  assert.equal(items.slice(1).every(item => item.status === "skip"), true);
  assert.equal(fake.calls.includes("stop"), true);
});
```

注意：第三个用例里 `fake` 在覆盖 `control` 之前已经创建；`Object.assign(fake.client, overrides)` 用的是同一个 `calls`，可以正常工作。

- [ ] **Step 2: 运行确认失败**

```bash
npm run test:selftest
```

预期：`ERR_MODULE_NOT_FOUND` 找不到 `src/selftest/runner.ts`。

- [ ] **Step 3: 实现 `runner.ts` 的预检与动作部分**

创建 `frontend/src/selftest/runner.ts`：

```ts
import {
  DEMO_TIMEOUT_MS,
  MOTION_ROWS,
  WIFI_SSID,
  adjustArmAngles,
  sameAngles,
} from "./core.ts";
import type { Cmd, Group, ItemResult, ItemStatus, RunState, StepResult } from "./core.ts";
import type { Client } from "./client.ts";

export interface ArmSnapshot {
  driver: string;
  angles: Record<string, unknown>;
}

export interface RunnerOptions {
  groups: Group[];
  speed: number;
  abortOnFailure: boolean;
  client: Client;
  cameraFrame: (timeoutMs: number) => Promise<boolean>;
  confirm: (itemId: string, message: string) => Promise<boolean>;
  isAborted: () => boolean;
  onUpdate: (items: ItemResult[], state: RunState) => void;
  onArmSnapshot: (snapshot: ArmSnapshot | null) => void;
  onCamera: (on: boolean) => void;
  now?: () => number;
}

export interface RunOutcome {
  items: ItemResult[];
  state: "finished" | "aborted";
  precheckError?: string;
  arm: ArmSnapshot | null;
}

interface ItemDef {
  id: string;
  name: string;
  group: Group;
}

const ITEM_DEFS: ItemDef[] = [
  ...MOTION_ROWS.map(row => ({id: row.id, name: row.label, group: "motion" as const})),
  {id: "camera_on", name: "摄像头 打开 / 显示", group: "camera"},
  {id: "camera_off", name: "摄像头关闭", group: "camera"},
  {id: "demo_grab", name: "自动抓网球", group: "demo"},
  {id: "wifi_scan", name: "WiFi 扫描", group: "wifi"},
  {id: "wifi_connect", name: `WiFi 连接 ${WIFI_SSID}`, group: "wifi"},
  {id: "wifi_verify", name: "WiFi 验证", group: "wifi"},
  {id: "arm_roundtrip", name: "舵机角度 调整 / 恢复", group: "arm"},
];

export function initialItems(groups: Group[]): ItemResult[] {
  return ITEM_DEFS.map(def => ({
    id: def.id,
    name: def.name,
    group: def.group,
    status: groups.includes(def.group) ? "pending" as ItemStatus : "skip" as ItemStatus,
    auto: null,
    manual: null,
    steps: [],
    elapsedMs: 0,
  }));
}

type Patch = (id: string, change: Partial<ItemResult>) => void;

export async function runSelfTest(opts: RunnerOptions): Promise<RunOutcome> {
  const now = opts.now ?? (() => Date.now());
  const items = initialItems(opts.groups);
  const find = (id: string) => items.find(item => item.id === id)!;
  const snapshot = () => items.map(item => ({...item, steps: item.steps.map(step => ({...step}))}));
  const emit = (state: RunState) => opts.onUpdate(snapshot(), state);
  const patch: Patch = (id, change) => {
    Object.assign(find(id), change);
    emit("running");
  };
  let arm: ArmSnapshot | null = null;
  let aborted = false;

  const markRemainingSkip = (reason: string) => {
    for (const item of items) {
      if (item.status === "pending") {
        item.status = "skip";
        item.error = reason;
      }
    }
  };
  const checkAbort = () => {
    if (!opts.isAborted()) return false;
    aborted = true;
    markRemainingSkip("已中止");
    return true;
  };

  emit("precheck");
  try {
    if (!(await opts.client.motorConnected())) {
      const reason = "预检失败：底盘未连接（motor.connected=false）";
      for (const item of items) {
        if (item.status === "pending") {
          item.status = "fail";
          item.error = reason;
        }
      }
      emit("aborted");
      return {items: snapshot(), state: "aborted", precheckError: reason, arm: null};
    }

    const hasTennis = opts.groups.includes("demo") ? await opts.client.demoHasTennis() : false;

    if (!checkAbort() && opts.groups.includes("motion")) {
      let skipRest = false;
      for (const row of MOTION_ROWS) {
        if (checkAbort()) break;
        if (skipRest) {
          patch(row.id, {status: "skip", error: "前一项失败，已跳过"});
          continue;
        }
        const passed = row.kind === "grab"
          ? await runGrab(row.id, opts, patch, now)
          : await runPair(row.id, row.first, row.second, opts, patch, now);
        if (!passed && opts.abortOnFailure) skipRest = true;
      }
    }

    if (!aborted && !checkAbort() && opts.groups.includes("camera")) {
      await runCamera(opts, patch, now);
    }

    if (!aborted && !checkAbort() && opts.groups.includes("demo")) {
      await runDemo(opts, patch, now, hasTennis);
    }

    if (!aborted && !checkAbort() && opts.groups.includes("wifi")) {
      await runWifi(opts, patch, now);
    }

    if (!aborted && !checkAbort() && opts.groups.includes("arm")) {
      arm = await runArm(opts, patch, now);
    }
  } finally {
    await opts.client.stop().catch(() => {});
    await opts.client.cameraClose().catch(() => {});
    opts.onCamera(false);
    if (arm) await opts.client.armWrite(arm.driver, arm.angles).catch(() => {});
  }

  if (aborted) markRemainingSkip("已中止");
  emit(aborted ? "aborted" : "finished");
  return {items: snapshot(), state: aborted ? "aborted" : "finished", arm};
}

async function runPair(
  id: string,
  first: Cmd,
  second: Cmd,
  opts: RunnerOptions,
  patch: Patch,
  now: () => number,
): Promise<boolean> {
  const started = now();
  const steps: StepResult[] = [];
  patch(id, {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});

  const runCmd = async (cmd: Cmd) => {
    const result = await opts.client.control(cmd.action, {...cmd.opts, speed: opts.speed, timeoutMs: cmd.timeoutMs});
    steps.push({name: cmd.name, ok: result.ok, elapsedMs: result.elapsedMs, error: result.error});
    patch(id, {steps: steps.map(step => ({...step})), elapsedMs: now() - started});
    if (!result.ok) await opts.client.stop().catch(() => {});
    return result.ok;
  };

  const firstOk = await runCmd(first);
  const secondOk = await runCmd(second);

  const auto: "pass" | "fail" = firstOk && secondOk ? "pass" : "fail";
  let status: ItemStatus = auto;
  let manual: "pass" | "fail" | null = null;
  if (auto === "pass") {
    patch(id, {status: "confirm", auto});
    manual = await opts.confirm(id, "动作是否到位？（距离 / 角度）") ? "pass" : "fail";
    status = manual;
  }
  patch(id, {status, auto, manual, elapsedMs: now() - started});
  return status === "pass";
}

async function runGrab(
  id: string,
  opts: RunnerOptions,
  patch: Patch,
  now: () => number,
): Promise<boolean> {
  const started = now();
  const steps: StepResult[] = [];
  patch(id, {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});

  const grab = await opts.client.control("grab", {speed: opts.speed, timeoutMs: 10000});
  steps.push({name: "抓取", ok: grab.ok, elapsedMs: grab.elapsedMs, error: grab.error});
  patch(id, {steps: steps.map(step => ({...step})), elapsedMs: now() - started});

  let held = false;
  if (grab.ok) {
    patch(id, {status: "confirm"});
    held = await opts.confirm(id, "是否夹住并抬起？");
  }

  const release = await opts.client.control("release", {speed: opts.speed, timeoutMs: 5000});
  steps.push({name: "放下", ok: release.ok, elapsedMs: release.elapsedMs, error: release.error});

  const auto: "pass" | "fail" = grab.ok && release.ok ? "pass" : "fail";
  const manual: "pass" | "fail" = held ? "pass" : "fail";
  const status: ItemStatus = auto === "pass" && held ? "pass" : "fail";
  patch(id, {
    status,
    auto,
    manual,
    steps: steps.map(step => ({...step})),
    elapsedMs: now() - started,
    error: grab.error ?? release.error,
  });
  return status === "pass";
}
```

- [ ] **Step 4: 先补上剩余组的占位实现，保证文件可编译**

Task 6/7 会替换这些占位。**本步骤只为了让当前 Task 的测试能跑**，在 `runner.ts` 末尾追加：

```ts
async function runCamera(opts: RunnerOptions, patch: Patch, now: () => number): Promise<void> {
  patch("camera_on", {status: "skip", elapsedMs: now() - now()});
  patch("camera_off", {status: "skip"});
  void opts;
}

async function runDemo(
  opts: RunnerOptions,
  patch: Patch,
  now: () => number,
  hasTennis: boolean,
): Promise<void> {
  patch("demo_grab", {status: "skip", elapsedMs: now() - now()});
  void opts;
  void hasTennis;
}

async function runWifi(opts: RunnerOptions, patch: Patch, now: () => number): Promise<void> {
  patch("wifi_scan", {status: "skip", elapsedMs: now() - now()});
  patch("wifi_connect", {status: "skip"});
  patch("wifi_verify", {status: "skip"});
  void opts;
}

async function runArm(opts: RunnerOptions, patch: Patch, now: () => number): Promise<ArmSnapshot | null> {
  patch("arm_roundtrip", {status: "skip", elapsedMs: now() - now()});
  void opts;
  return null;
}
```

- [ ] **Step 5: 运行测试**

```bash
npm run test:selftest
```

预期：`runner.test.ts` 的 3 个用例通过；`core`/`history` 继续通过。

---

## Task 6: runner 摄像头与自动抓网球

**Files:**
- Modify: `frontend/tests/runner.test.ts`（追加用例）
- Modify: `frontend/src/selftest/runner.ts`（替换占位）

- [ ] **Step 1: 追加失败测试**

在 `frontend/tests/runner.test.ts` 末尾追加：

```ts
test("摄像头组：打开→首帧→确认→关闭并校验", async () => {
  const {fake, outcome} = await run(["camera"]);
  assert.deepEqual(fake.calls, ["camera.open", "camera.close"]);
  const items = outcome.items.filter(item => item.group === "camera");
  assert.deepEqual(items.map(item => item.status), ["pass", "pass"]);
});

test("自动抓网球：状态 done 且人工确认后通过", async () => {
  const {fake, outcome} = await run(["demo"]);
  assert.equal(fake.calls.includes("demo.start"), true);
  const item = outcome.items.find(item => item.id === "demo_grab")!;
  assert.equal(item.status, "pass");
  assert.equal(fake.calls.includes("demo.stop"), false);
});

test("自动抓网球：90s 超时后停止并失败", async () => {
  let clock = 0;
  const {fake, outcome} = await run(["demo"], {
    demoStatus: async () => ({ok: true, state: "running"}),
    sleep: async (ms) => { clock += ms; },
  }, {
    now: () => clock,
    confirm: () => false,
  });
  const item = outcome.items.find(item => item.id === "demo_grab")!;
  assert.equal(item.status, "fail");
  assert.match(item.error ?? "", /超时/);
  assert.equal(fake.calls.includes("demo.stop"), true);
});
```

- [ ] **Step 2: 运行确认失败**

```bash
npm run test:selftest
```

预期：新增 3 个用例失败（占位实现把项标记为 skip）。

- [ ] **Step 3: 替换 `runCamera` 占位**

把 `runner.ts` 里的 `runCamera` 整体替换为：

```ts
async function runCamera(opts: RunnerOptions, patch: Patch, now: () => number): Promise<void> {
  const started = now();
  patch("camera_on", {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});
  const open = await opts.client.cameraOpen();
  let frame = false;
  if (open.ok) frame = await opts.cameraFrame(8000);

  const steps: StepResult[] = [
    {name: "打开摄像头", ok: open.ok, elapsedMs: 0, error: open.error},
    {name: "首帧到达", ok: frame, elapsedMs: 0},
  ];
  if (open.ok && frame) {
    patch("camera_on", {status: "confirm", auto: "pass", steps});
    const confirmed = await opts.confirm("camera_on", "画面是否正常？（网页与板载屏）");
    patch("camera_on", {
      status: confirmed ? "pass" : "fail",
      auto: "pass",
      manual: confirmed ? "pass" : "fail",
      steps,
      elapsedMs: now() - started,
    });
  } else {
    patch("camera_on", {
      status: "fail",
      auto: "fail",
      steps,
      elapsedMs: now() - started,
      error: open.error ?? "未收到画面",
    });
  }

  const closeStarted = now();
  patch("camera_off", {status: "running", steps: []});
  const close = await opts.client.cameraClose();
  const status = await opts.client.cameraStatus();
  const ok = close.ok && status.ok && status.camera_on === false;
  patch("camera_off", {
    status: ok ? "pass" : "fail",
    auto: ok ? "pass" : "fail",
    manual: null,
    steps: [{name: "关闭并确认 camera_on=false", ok, elapsedMs: now() - closeStarted, error: close.error}],
    elapsedMs: now() - closeStarted,
  });
}
```

- [ ] **Step 4: 替换 `runDemo` 占位**

把 `runner.ts` 里的 `runDemo` 整体替换为：

```ts
async function runDemo(
  opts: RunnerOptions,
  patch: Patch,
  now: () => number,
  hasTennis: boolean,
): Promise<void> {
  const id = "demo_grab";
  if (!hasTennis) {
    patch(id, {status: "skip", error: "未找到 tennis 模型"});
    return;
  }

  const started = now();
  patch(id, {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});
  const start = await opts.client.demoStart();
  if (!start.ok) {
    patch(id, {status: "fail", auto: "fail", error: start.error ?? "启动失败", elapsedMs: now() - started});
    return;
  }

  const deadline = started + DEMO_TIMEOUT_MS;
  let state = "running";
  let message = "";
  while (now() < deadline) {
    if (opts.isAborted()) break;
    const status = await opts.client.demoStatus();
    state = status.state;
    if (status.message) message = status.message;
    if (["done", "failed", "aborted", "idle"].includes(state)) break;
    await opts.client.sleep(1000);
  }

  if (opts.isAborted()) {
    await opts.client.demoStop().catch(() => {});
    patch(id, {status: "skip", error: "已中止", elapsedMs: now() - started});
    return;
  }

  const timedOut = now() >= deadline || !["done", "failed", "aborted", "idle"].includes(state);
  if (timedOut) await opts.client.demoStop().catch(() => {});

  const auto: "pass" | "fail" = timedOut || state === "failed" || state === "aborted" ? "fail" : "pass";
  const timeoutError = timedOut ? `超时 ${DEMO_TIMEOUT_MS / 1000}s，已停止` : undefined;
  if (auto === "fail") {
    patch(id, {
      status: "fail",
      auto,
      manual: null,
      elapsedMs: now() - started,
      error: timeoutError ?? (message || "脚本执行失败"),
    });
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
}
```

- [ ] **Step 5: 运行测试**

```bash
npm run test:selftest
```

预期：runner 6 个用例全部通过。

---

## Task 7: runner WiFi、舵机与清理

**Files:**
- Modify: `frontend/tests/runner.test.ts`（追加用例）
- Modify: `frontend/src/selftest/runner.ts`（替换占位）

- [ ] **Step 1: 追加失败测试**

在 `frontend/tests/runner.test.ts` 末尾追加：

```ts
test("WiFi 组：扫描→连接→验证 全部通过", async () => {
  const {outcome} = await run(["wifi"]);
  const items = outcome.items.filter(item => item.group === "wifi");
  assert.deepEqual(items.map(item => item.status), ["pass", "pass", "pass"]);
});

test("WiFi 组：连接失败时 verify 跳过", async () => {
  const {outcome} = await run(["wifi"], {
    wifiConnect: async () => ({ok: false, error: "关联超时"}),
  });
  const items = outcome.items.filter(item => item.group === "wifi");
  assert.deepEqual(items.map(item => item.status), ["pass", "fail", "skip"]);
});

test("舵机组：调整后恢复原始角度，最终配置与运行前一致", async () => {
  let angles: Record<string, unknown> = {gripper_open: 150, gripper_close: 90};
  const writes: Record<string, unknown>[] = [];
  const {outcome} = await run(["arm"], {
    armRead: async () => ({ok: true, driver: "zp10s", angles: structuredClone(angles)}),
    armWrite: async (_driver, next) => {
      writes.push(structuredClone(next));
      angles = structuredClone(next);
      return {ok: true};
    },
  });
  const item = outcome.items.find(entry => entry.id === "arm_roundtrip")!;
  assert.equal(item.status, "pass");
  assert.deepEqual(angles, {gripper_open: 150, gripper_close: 90});
  assert.equal(writes.length, 3);
  assert.equal(writes[0].gripper_open, 160);
  assert.equal(writes[1].gripper_open, 150);
});

test("舵机组：调整写入失败也会恢复原始角度并记失败", async () => {
  let angles: Record<string, unknown> = {gripper_open: 150, gripper_close: 90};
  let writeCount = 0;
  const {outcome} = await run(["arm"], {
    armRead: async () => ({ok: true, driver: "zp10s", angles: structuredClone(angles)}),
    armWrite: async (_driver, next) => {
      writeCount += 1;
      if (writeCount === 1) return {ok: false, error: "写入失败"};
      angles = structuredClone(next);
      return {ok: true};
    },
  });
  const item = outcome.items.find(entry => entry.id === "arm_roundtrip")!;
  assert.equal(item.status, "fail");
  assert.deepEqual(angles, {gripper_open: 150, gripper_close: 90});
});

test("中止：未开始的项全部跳过，清理命令仍执行", async () => {
  const {fake, outcome} = await run(["motion", "camera"], {}, {isAborted: () => true});
  assert.equal(outcome.state, "aborted");
  assert.equal(outcome.items.every(item => item.status === "skip"), true);
  assert.equal(fake.calls.includes("stop"), true);
  assert.equal(fake.calls.includes("camera.close"), true);
});
```

- [ ] **Step 2: 运行确认失败**

```bash
npm run test:selftest
```

预期：新增 5 个用例失败。

- [ ] **Step 3: 替换 `runWifi` 占位**

```ts
async function runWifi(opts: RunnerOptions, patch: Patch, now: () => number): Promise<void> {
  const scanStarted = now();
  patch("wifi_scan", {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});
  const scan = await opts.client.wifiScan();
  const hit = scan.list.find(net => net.ssid === WIFI_SSID);
  const scanOk = scan.ok && !!hit;
  patch("wifi_scan", {
    status: scanOk ? "pass" : "fail",
    auto: scanOk ? "pass" : "fail",
    manual: null,
    steps: [{
      name: `扫描到 ${WIFI_SSID}`,
      ok: scanOk,
      elapsedMs: now() - scanStarted,
      detail: hit ? `信号 ${hit.signal}dBm` : `共 ${scan.list.length} 个网络`,
    }],
    elapsedMs: now() - scanStarted,
    error: scanOk ? undefined : (scan.error ?? `未扫描到 ${WIFI_SSID}`),
  });

  const connectStarted = now();
  patch("wifi_connect", {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});
  const connect = await opts.client.wifiConnect(WIFI_SSID, "");
  const connectOk = connect.ok && !!connect.ip;
  patch("wifi_connect", {
    status: connectOk ? "pass" : "fail",
    auto: connectOk ? "pass" : "fail",
    manual: null,
    steps: [{
      name: "连接（无密码）",
      ok: connectOk,
      elapsedMs: now() - connectStarted,
      detail: connect.ip,
    }],
    elapsedMs: now() - connectStarted,
    error: connect.error,
  });

  if (!connectOk) {
    patch("wifi_verify", {status: "skip", error: "连接失败，未验证"});
    return;
  }

  const verifyStarted = now();
  patch("wifi_verify", {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});
  const status = await opts.client.wifiStatus();
  const verifyOk = status.ok && status.ssid === WIFI_SSID && !!status.ip;
  patch("wifi_verify", {
    status: verifyOk ? "pass" : "fail",
    auto: verifyOk ? "pass" : "fail",
    manual: null,
    steps: [{
      name: `确认 SSID=${WIFI_SSID} 且拿到 IP`,
      ok: verifyOk,
      elapsedMs: now() - verifyStarted,
      detail: status.ip,
    }],
    elapsedMs: now() - verifyStarted,
    error: verifyOk ? undefined : `当前 SSID=${status.ssid ?? "无"}`,
  });
}
```

- [ ] **Step 4: 替换 `runArm` 占位**

```ts
async function runArm(opts: RunnerOptions, patch: Patch, now: () => number): Promise<ArmSnapshot | null> {
  const id = "arm_roundtrip";
  const started = now();
  const steps: StepResult[] = [];
  patch(id, {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});

  const read = await opts.client.armRead();
  if (!read.ok) {
    patch(id, {status: "fail", auto: "fail", error: read.error ?? "读取角度失败", elapsedMs: now() - started});
    return null;
  }

  const original: ArmSnapshot = {driver: read.driver, angles: read.angles};
  opts.onArmSnapshot(original);
  steps.push({name: "读取原始角度", ok: true, elapsedMs: 0, detail: read.driver});
  patch(id, {steps: steps.map(step => ({...step}))});

  const allowed = await opts.confirm(id, "确认机械臂周围无障碍，开始调整角度？");
  if (!allowed) {
    patch(id, {status: "skip", steps: steps.map(step => ({...step})), error: "操作员取消"});
    return original;
  }

  let writeOk = false;
  let adjustError: string | undefined;
  try {
    const adjust = adjustArmAngles(read.driver, original.angles);
    const write = await opts.client.armWrite(read.driver, adjust.next);
    writeOk = write.ok;
    steps.push({
      name: `调整 ${adjust.key}: ${adjust.from} → ${adjust.to}`,
      ok: write.ok,
      elapsedMs: 0,
      error: write.error,
    });
    patch(id, {steps: steps.map(step => ({...step}))});

    if (write.ok) {
      await opts.client.sleep(800);
      const verify = await opts.client.armRead();
      const changed = verify.ok && sameAngles(verify.angles, adjust.next);
      steps.push({name: "确认新角度已生效", ok: changed, elapsedMs: 0});
      patch(id, {steps: steps.map(step => ({...step}))});
    }
  } catch (e) {
    adjustError = e instanceof Error ? e.message : String(e);
    steps.push({name: "调整角度", ok: false, elapsedMs: 0, error: adjustError});
  }

  let restored = false;
  try {
    const write = await opts.client.armWrite(read.driver, original.angles);
    steps.push({name: "写回原始角度", ok: write.ok, elapsedMs: 0, error: write.error});
    const verify = await opts.client.armRead();
    restored = write.ok && verify.ok && sameAngles(verify.angles, original.angles);
    steps.push({name: "确认已恢复", ok: restored, elapsedMs: 0});
  } catch (e) {
    steps.push({
      name: "恢复原始角度",
      ok: false,
      elapsedMs: 0,
      error: e instanceof Error ? e.message : String(e),
    });
  }

  const allOk = writeOk && restored && steps.every(step => step.ok);
  const error = !restored
    ? "恢复后角度与原始值不一致"
    : (adjustError ?? (writeOk ? undefined : "调整角度失败"));
  patch(id, {
    status: allOk ? "pass" : "fail",
    auto: allOk ? "pass" : "fail",
    manual: null,
    steps: steps.map(step => ({...step})),
    elapsedMs: now() - started,
    error,
  });
  return original;
}
```

- [ ] **Step 5: 运行全部测试**

```bash
npm run test:selftest
```

预期：`core` 6 + `history` 4 + `runner` 11 全部通过。

- [ ] **Step 6: 类型检查**

```bash
npx tsc -b
```

预期：无输出、退出码 0。

---

## Task 8: 测试项列表组件

**Files:**
- Create: `frontend/src/selftest/components/ItemList.tsx`

- [ ] **Step 1: 实现 `ItemList.tsx`**

```tsx
import {useViewportScale} from "../../hooks/useViewportScale";
import type { ItemResult, ItemStatus } from "../core.ts";

const GROUP_LABELS: Record<string, string> = {
  motion: "1. 动作",
  camera: "2. 摄像头",
  demo: "3. 自动抓网球",
  wifi: "4. WiFi（可选）",
  arm: "5. 舵机角度（可选）",
};

const STATUS_TEXT: Record<ItemStatus, string> = {
  pending: "待测",
  running: "运行中",
  confirm: "待确认",
  pass: "通过",
  fail: "失败",
  skip: "跳过",
  unconfirmed: "未确认",
};

const STATUS_COLOR: Record<ItemStatus, string> = {
  pending: "var(--color-text-dim)",
  running: "var(--color-primary)",
  confirm: "#f59e0b",
  pass: "var(--color-success)",
  fail: "var(--color-danger)",
  skip: "var(--color-text-dim)",
  unconfirmed: "#f59e0b",
};

const ItemList = ({items}: {items: ItemResult[]}) => {
  const {scalePx} = useViewportScale();
  let lastGroup = "";

  return (
    <div>
      {items.map(item => {
        const header = item.group !== lastGroup ? GROUP_LABELS[item.group] : null;
        lastGroup = item.group;
        return (
          <div key={item.id}>
            {header && (
              <div style={{
                fontSize: scalePx(12),
                color: "var(--color-text-dim)",
                margin: `${scalePx(10)} 0 ${scalePx(4)}`,
              }}>
                {header}
              </div>
            )}
            <div style={{padding: `${scalePx(6)} 0`, borderBottom: "1px solid var(--color-border-light)"}}>
              <div style={{display: "flex", alignItems: "center", gap: scalePx(8)}}>
                <span style={{flex: 1, fontSize: scalePx(14), fontWeight: 600}}>{item.name}</span>
                {item.elapsedMs > 0 && (
                  <span style={{fontSize: scalePx(11), color: "var(--color-text-dim)"}}>
                    {(item.elapsedMs / 1000).toFixed(1)}s
                  </span>
                )}
                <span style={{
                  fontSize: scalePx(12),
                  color: STATUS_COLOR[item.status],
                  fontWeight: 600,
                  minWidth: scalePx(48),
                  textAlign: "right",
                }}>
                  {STATUS_TEXT[item.status]}
                </span>
              </div>

              {item.steps.length > 0 && (
                <div style={{marginTop: scalePx(3)}}>
                  {item.steps.map((step, index) => (
                    <div
                      key={index}
                      style={{
                        fontSize: scalePx(11),
                        color: step.ok ? "var(--color-text-muted)" : "var(--color-danger)",
                      }}
                    >
                      {step.ok ? "✓" : "✗"} {step.name}
                      {step.detail ? `（${step.detail}）` : ""}
                      {step.error ? ` — ${step.error}` : ""}
                    </div>
                  ))}
                </div>
              )}

              {item.error && (
                <div style={{fontSize: scalePx(11), color: "var(--color-danger)", marginTop: 2}}>
                  {item.error}
                </div>
              )}
            </div>
          </div>
        );
      })}
    </div>
  );
};

export default ItemList;
```

- [ ] **Step 2: 类型检查**

```bash
npx tsc -b
```

预期：无输出、退出码 0。

---

## Task 9: 历史与备注面板

**Files:**
- Create: `frontend/src/selftest/components/HistoryPanel.tsx`

- [ ] **Step 1: 实现 `HistoryPanel.tsx`**

```tsx
import {useViewportScale} from "../../hooks/useViewportScale";
import type { SelfTestRecord } from "../core.ts";

interface HistoryPanelProps {
  records: SelfTestRecord[];
  remark: string;
  saved: boolean;
  canSave: boolean;
  onChangeRemark: (value: string) => void;
  onSave: () => void;
  onExport: (record: SelfTestRecord) => void;
  onClear: () => void;
}

const formatTime = (ts: number) => new Date(ts).toLocaleString("zh-CN", {hour12: false});

const HistoryPanel = ({
  records, remark, saved, canSave, onChangeRemark, onSave, onExport, onClear,
}: HistoryPanelProps) => {
  const {scalePx} = useViewportScale();

  return (
    <div>
      <div style={{fontSize: scalePx(14), fontWeight: 700, marginTop: scalePx(14), marginBottom: scalePx(6)}}>
        本次测试备注
      </div>
      <textarea
        value={remark}
        onChange={event => onChangeRemark(event.target.value)}
        placeholder="例如：左转 90° 略偏、夹爪正常"
        rows={3}
        style={{
          width: "100%",
          boxSizing: "border-box",
          padding: scalePx(8),
          borderRadius: "var(--radius-sm)",
          border: "1px solid var(--color-border)",
          background: "var(--color-bg-card)",
          color: "var(--color-text)",
          fontSize: scalePx(13),
          resize: "vertical",
        }}
      />
      <button
        onClick={onSave}
        disabled={!canSave}
        style={{
          width: "100%",
          marginTop: scalePx(8),
          padding: `${scalePx(10)} 0`,
          border: "none",
          borderRadius: "var(--radius-md)",
          background: saved ? "var(--color-success)" : "var(--color-primary)",
          color: "#fff",
          fontSize: scalePx(14),
          fontWeight: 600,
          cursor: canSave ? "pointer" : "not-allowed",
          opacity: canSave ? 1 : 0.5,
        }}
      >
        {saved ? "✓ 已保存" : "保存本次记录"}
      </button>

      <div style={{display: "flex", alignItems: "center", marginTop: scalePx(16), marginBottom: scalePx(4)}}>
        <span style={{flex: 1, fontSize: scalePx(14), fontWeight: 700}}>测试历史（{records.length}）</span>
        {records.length > 0 && (
          <button
            onClick={onClear}
            style={{
              border: "none",
              background: "none",
              color: "var(--color-danger)",
              fontSize: scalePx(12),
              cursor: "pointer",
              padding: scalePx(4),
            }}
          >
            清空
          </button>
        )}
      </div>

      {records.length === 0 && (
        <div style={{fontSize: scalePx(12), color: "var(--color-text-dim)"}}>暂无记录</div>
      )}

      {records.map(record => (
        <div key={record.id} style={{padding: `${scalePx(6)} 0`, borderBottom: "1px solid var(--color-border-light)"}}>
          <div style={{display: "flex", alignItems: "center", gap: scalePx(6)}}>
            <span style={{flex: 1, fontSize: scalePx(12), color: "var(--color-text-muted)"}}>{formatTime(record.ts)}</span>
            <span style={{fontSize: scalePx(11), color: "var(--color-text-dim)"}}>
              通过 {record.summary.pass} / 失败 {record.summary.fail}
              {record.summary.skip > 0 ? ` / 跳过 ${record.summary.skip}` : ""}
              {record.summary.unconfirmed > 0 ? ` / 未确认 ${record.summary.unconfirmed}` : ""}
            </span>
          </div>
          {record.remark && (
            <div style={{fontSize: scalePx(12), color: "var(--color-text)", marginTop: 2}}>{record.remark}</div>
          )}
          <button
            onClick={() => onExport(record)}
            style={{
              marginTop: scalePx(3),
              border: "1px solid var(--color-border)",
              background: "var(--color-bg-card)",
              color: "var(--color-text-dim)",
              borderRadius: "var(--radius-sm)",
              fontSize: scalePx(11),
              padding: `${scalePx(2)} ${scalePx(8)}`,
              cursor: "pointer",
            }}
          >
            导出 JSON
          </button>
        </div>
      ))}
    </div>
  );
};

export default HistoryPanel;
```

- [ ] **Step 2: 类型检查**

```bash
npx tsc -b
```

预期：无输出、退出码 0。

---

## Task 10: 页面组装

**Files:**
- Create: `frontend/src/pages/SelfTestPage.tsx`

- [ ] **Step 1: 实现页面**

创建 `frontend/src/pages/SelfTestPage.tsx`：

```tsx
import {useEffect, useMemo, useRef, useState} from "react";
import Header from "../components/Header";
import Card from "../components/Card";
import ConfirmDialog from "../components/ConfirmDialog";
import {useViewportScale} from "../hooks/useViewportScale";
import ItemList from "../selftest/components/ItemList.tsx";
import HistoryPanel from "../selftest/components/HistoryPanel.tsx";
import {createClient} from "../selftest/client.ts";
import {runSelfTest} from "../selftest/runner.ts";
import type { ArmSnapshot } from "../selftest/runner.ts";
import {appendRecord, clearHistory, exportFilename, loadHistory} from "../selftest/history.ts";
import {
  summarizeItems,
  type Group,
  type ItemResult,
  type RunState,
  type SelfTestRecord,
} from "../selftest/core.ts";

const PAGE_STYLE = {
  minHeight: "100dvh",
  overflowY: "auto",
  "--color-bg": "#f2f2f7",
  "--color-bg-card": "#ffffff",
  "--color-bg-elevated": "#e5e5ea",
  "--color-text": "#1c1c1e",
  "--color-text-dim": "#8e8e93",
  "--color-text-muted": "#6e6e73",
  "--color-border": "#d1d1d6",
  "--color-border-light": "rgba(0,0,0,0.08)",
  "--color-primary": "#007aff",
  "--color-success": "#34c759",
  "--color-danger": "#ff3b30",
  color: "#1c1c1e",
  background: "#f2f2f7",
} as React.CSSProperties;

const checkboxRowStyle = (scalePx: (value: number) => number): React.CSSProperties => ({
  display: "flex",
  alignItems: "center",
  gap: scalePx(8),
  padding: `${scalePx(5)} 0`,
  fontSize: scalePx(13),
  cursor: "pointer",
});

const SelfTestPage = () => {
  const {scalePx} = useViewportScale();
  const client = useMemo(() => createClient(), []);

  const [checked, setChecked] = useState(false);
  const [enableWifi, setEnableWifi] = useState(false);
  const [enableArm, setEnableArm] = useState(false);
  const [abortOnFailure, setAbortOnFailure] = useState(true);
  const [speed, setSpeed] = useState(25);

  const [runState, setRunState] = useState<RunState>("idle");
  const [items, setItems] = useState<ItemResult[]>([]);
  const [precheckError, setPrecheckError] = useState("");
  const [info, setInfo] = useState({mac: "", ip: "", version: ""});

  const [confirmMessage, setConfirmMessage] = useState<string | null>(null);
  const [cameraSrc, setCameraSrc] = useState("");
  const [armSnapshot, setArmSnapshot] = useState<ArmSnapshot | null>(null);

  const [remark, setRemark] = useState("");
  const [saved, setSaved] = useState(false);
  const [history, setHistory] = useState<SelfTestRecord[]>(() => loadHistory(window.localStorage));
  const [showClear, setShowClear] = useState(false);

  const abortRef = useRef(false);
  const confirmRef = useRef<((ok: boolean) => void) | null>(null);
  const frameWaiterRef = useRef<((ok: boolean) => void) | null>(null);

  const running = runState === "precheck" || runState === "running";

  useEffect(() => {
    let alive = true;
    Promise.all([client.systemInfo(), client.version()])
      .then(([system, version]) => {
        if (alive) setInfo({mac: system.mac, ip: system.ip, version});
      })
      .catch(() => {});
    return () => { alive = false; };
  }, [client]);

  const askConfirm = (_itemId: string, message: string) => new Promise<boolean>(resolve => {
    confirmRef.current = resolve;
    setConfirmMessage(message);
  });

  const answerConfirm = (ok: boolean) => {
    confirmRef.current?.(ok);
    confirmRef.current = null;
    setConfirmMessage(null);
  };

  const cameraFrame = (timeoutMs: number) => new Promise<boolean>(resolve => {
    let done = false;
    const finish = (ok: boolean) => {
      if (done) return;
      done = true;
      frameWaiterRef.current = null;
      if (!ok) setCameraSrc("");
      resolve(ok);
    };
    frameWaiterRef.current = finish;
    setCameraSrc(`/api/camera/stream?fps=10&t=${Date.now()}`);
    window.setTimeout(() => finish(false), timeoutMs);
  });

  const start = async () => {
    if (!checked || running) return;
    abortRef.current = false;
    setPrecheckError("");
    setSaved(false);
    setRemark("");

    const groups: Group[] = ["motion", "camera", "demo"];
    if (enableWifi) groups.push("wifi");
    if (enableArm) groups.push("arm");

    const outcome = await runSelfTest({
      groups,
      speed,
      abortOnFailure,
      client,
      cameraFrame,
      confirm: askConfirm,
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
  };

  const abort = () => {
    abortRef.current = true;
    answerConfirm(false);
    void client.stop();
    void client.demoStop();
  };

  const restoreArm = () => {
    if (!armSnapshot) return;
    void client.armWrite(armSnapshot.driver, armSnapshot.angles);
  };

  const saveRecord = () => {
    if (items.length === 0) return;
    const ts = Date.now();
    const summary = summarizeItems(items);
    const record: SelfTestRecord = {
      id: `${ts}-${Math.random().toString(36).slice(2, 8)}`,
      ts,
      mac: info.mac,
      version: info.version,
      ip: info.ip,
      speed,
      items,
      summary: {
        pass: summary.pass,
        fail: summary.fail,
        skip: summary.skip,
        unconfirmed: summary.unconfirmed,
      },
      remark: remark.trim(),
    };
    setHistory(appendRecord(window.localStorage, record));
    setSaved(true);
  };

  const exportRecord = (record: SelfTestRecord) => {
    const blob = new Blob([JSON.stringify(record, null, 2)], {type: "application/json"});
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = exportFilename(record.mac, record.ts);
    link.click();
    URL.revokeObjectURL(url);
  };

  const clearAll = () => {
    clearHistory(window.localStorage);
    setHistory([]);
    setShowClear(false);
  };

  const summary = summarizeItems(items);
  const canSave = (runState === "finished" || runState === "aborted") && items.length > 0 && !saved;

  return (
    <div style={PAGE_STYLE}>
      <Header
        title="自动测试"
        subtitle={info.mac ? `MAC ${info.mac}` : undefined}
        showClose
        closeTo="/settings"
      />
      <div style={{padding: scalePx(16), maxWidth: scalePx(420), margin: "0 auto"}}>
        <Card marginBottom={10}>
          <div style={{display: "flex", justifyContent: "space-between", fontSize: scalePx(12), color: "var(--color-text-dim)"}}>
            <span>IP</span>
            <span style={{fontFamily: "monospace", color: "var(--color-text)"}}>{info.ip || "—"}</span>
          </div>
          <div style={{display: "flex", justifyContent: "space-between", fontSize: scalePx(12), color: "var(--color-text-dim)", marginTop: scalePx(3)}}>
            <span>版本</span>
            <span style={{fontFamily: "monospace", color: "var(--color-text)"}}>{info.version || "—"}</span>
          </div>
        </Card>

        <Card marginBottom={10}>
          <label style={checkboxRowStyle(scalePx)}>
            <input
              type="checkbox"
              checked={checked}
              onChange={event => setChecked(event.target.checked)}
              disabled={running}
              style={{accentColor: "var(--color-primary)"}}
            />
            我已确认车前 1 米内无障碍、机械臂周围无障碍
          </label>
          <label style={checkboxRowStyle(scalePx)}>
            <input
              type="checkbox"
              checked={enableWifi}
              onChange={event => setEnableWifi(event.target.checked)}
              disabled={running}
              style={{accentColor: "var(--color-primary)"}}
            />
            测试 WiFi（连接 Tsinghua-Dongsheng）
          </label>
          <label style={checkboxRowStyle(scalePx)}>
            <input
              type="checkbox"
              checked={enableArm}
              onChange={event => setEnableArm(event.target.checked)}
              disabled={running}
              style={{accentColor: "var(--color-primary)"}}
            />
            测试舵机角度（调整后自动恢复）
          </label>
          <label style={checkboxRowStyle(scalePx)}>
            <input
              type="checkbox"
              checked={abortOnFailure}
              onChange={event => setAbortOnFailure(event.target.checked)}
              disabled={running}
              style={{accentColor: "var(--color-primary)"}}
            />
            动作失败后继续（默认失败即跳过后续动作）
          </label>
          <div style={{marginTop: scalePx(6)}}>
            <div style={{display: "flex", justifyContent: "space-between", fontSize: scalePx(13), marginBottom: 2}}>
              <span>测试速度</span>
              <span style={{color: "var(--color-success)", fontWeight: 700}}>{speed}%</span>
            </div>
            <input
              type="range"
              min={10}
              max={40}
              step={5}
              value={speed}
              onChange={event => setSpeed(Number(event.target.value))}
              disabled={running}
              style={{width: "100%", accentColor: "var(--color-primary)"}}
            />
          </div>
        </Card>

        {precheckError && (
          <Card marginBottom={10} style={{background: "rgba(255,59,48,0.08)"}}>
            <div style={{fontSize: scalePx(13), color: "var(--color-danger)", fontWeight: 600}}>{precheckError}</div>
          </Card>
        )}

        <div style={{display: "flex", gap: scalePx(8), marginBottom: scalePx(10)}}>
          <button
            onClick={start}
            disabled={!checked || running}
            style={{
              flex: 1,
              padding: `${scalePx(11)} 0`,
              border: "none",
              borderRadius: "var(--radius-md)",
              background: "var(--color-primary)",
              color: "#fff",
              fontSize: scalePx(15),
              fontWeight: 700,
              cursor: !checked || running ? "not-allowed" : "pointer",
              opacity: !checked || running ? 0.5 : 1,
            }}
          >
            {running ? (runState === "precheck" ? "预检中…" : "测试进行中…") : "开始测试"}
          </button>
          {running && (
            <button
              onClick={abort}
              style={{
                padding: `0 ${scalePx(14)}`,
                border: "none",
                borderRadius: "var(--radius-md)",
                background: "var(--color-danger)",
                color: "#fff",
                fontSize: scalePx(14),
                fontWeight: 600,
                cursor: "pointer",
              }}
            >
              中止
            </button>
          )}
          <button
            onClick={restoreArm}
            disabled={!armSnapshot}
            style={{
              padding: `0 ${scalePx(12)}`,
              border: "1px solid var(--color-border)",
              borderRadius: "var(--radius-md)",
              background: "var(--color-bg-card)",
              color: armSnapshot ? "var(--color-text)" : "var(--color-text-dim)",
              fontSize: scalePx(12),
              cursor: armSnapshot ? "pointer" : "not-allowed",
              opacity: armSnapshot ? 1 : 0.5,
            }}
          >
            恢复机械臂
          </button>
        </div>

        {cameraSrc && (
          <Card marginBottom={10}>
            <img
              src={cameraSrc}
              alt="摄像头画面"
              onLoad={() => frameWaiterRef.current?.(true)}
              onError={() => frameWaiterRef.current?.(false)}
              style={{width: "100%", borderRadius: scalePx(8), display: "block"}}
            />
          </Card>
        )}

        <Card marginBottom={10}>
          {items.length === 0 ? (
            <div style={{fontSize: scalePx(13), color: "var(--color-text-dim)", lineHeight: 1.6}}>
              勾选上方安全确认后点击“开始测试”。测试会按顺序执行动作、摄像头、自动抓网球；WiFi 与舵机按勾选执行。
            </div>
          ) : (
            <>
              <div style={{display: "flex", justifyContent: "space-between", fontSize: scalePx(12), marginBottom: scalePx(4)}}>
                <span style={{color: "var(--color-text-dim)"}}>结果</span>
                <span style={{color: summary.fail > 0 ? "var(--color-danger)" : summary.ok ? "var(--color-success)" : "var(--color-text-muted)"}}>
                  通过 {summary.pass} / 失败 {summary.fail} / 跳过 {summary.skip}
                  {summary.unconfirmed > 0 ? ` / 未确认 ${summary.unconfirmed}` : ""}
                </span>
              </div>
              <ItemList items={items}/>
            </>
          )}
        </Card>

        {(runState === "finished" || runState === "aborted" || history.length > 0) && (
          <Card marginBottom={10}>
            <HistoryPanel
              records={history}
              remark={remark}
              saved={saved}
              canSave={canSave}
              onChangeRemark={setRemark}
              onSave={saveRecord}
              onExport={exportRecord}
              onClear={() => setShowClear(true)}
            />
          </Card>
        )}

        <div style={{height: scalePx(24)}}/>
      </div>

      <ConfirmDialog
        open={confirmMessage !== null}
        title="请确认"
        message={confirmMessage ?? ""}
        confirmText="通过"
        cancelText="失败"
        onConfirm={() => answerConfirm(true)}
        onCancel={() => answerConfirm(false)}
      />

      <ConfirmDialog
        open={showClear}
        title="清空测试历史"
        message="确定要清空本机保存的全部测试记录吗？"
        confirmText="清空"
        cancelText="取消"
        danger
        onConfirm={clearAll}
        onCancel={() => setShowClear(false)}
      />
    </div>
  );
};

export default SelfTestPage;
```

- [ ] **Step 2: 类型检查**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00/frontend"
npx tsc -b
```

预期：无输出、退出码 0。若报 `React` 命名空间未找到，把 `PAGE_STYLE` 的类型改成 `import type { CSSProperties } from "react"` 并用 `CSSProperties`；不要引入未使用的 import。

- [ ] **Step 3: 启动开发服务器人工冒烟**

```bash
npm run dev
```

浏览器打开终端提示的地址 → `/settings` 应能看到新入口（Task 11 添加）；若尚未添加，可直接访问 `/self-test`。检查：安全确认未勾选时“开始测试”禁用；勾选后可点击；页面无控制台报错。

---

## Task 11: 路由与设置入口

**Files:**
- Modify: `frontend/src/App.tsx:13`（import）与 `:49`（路由）
- Modify: `frontend/src/pages/SettingsPage.tsx:40-45`（items）

- [ ] **Step 1: 加路由**

在 `frontend/src/App.tsx` 的 import 区加：

```tsx
import SelfTestPage from "./pages/SelfTestPage.tsx";
```

在 `<Route path="/speed-config" ... />` 之后加：

```tsx
<Route path="/self-test" element={<SelfTestPage />} />
```

- [ ] **Step 2: 加设置项**

修改 `frontend/src/pages/SettingsPage.tsx` 的 `items` 数组，在“固件升级”前插入：

```tsx
        {icon: "🧪", title: "自动测试", desc: "逐项检查动作、摄像头、抓取、网络与舵机", path: "/self-test"},
```

完整数组应为：

```tsx
    const items = [
        {icon: "📶", title: "WiFi 配置", desc: "扫描并连接无线网络", path: "/wifi"},
        {icon: "🔧", title: "舵机角度配置", desc: "调整抓取序列", path: "/arm-angles"},
        {icon: "🚗", title: "行驶速度", desc: "调整前进和转向速度百分比", path: "/speed-config"},
        {icon: "🧪", title: "自动测试", desc: "逐项检查动作、摄像头、抓取、网络与舵机", path: "/self-test"},
        {icon: "⬆️", title: "固件升级", desc: "检查更新并升级系统固件", path: "/ota"},
    ];
```

- [ ] **Step 3: 类型检查 + 全部测试**

```bash
npx tsc -b
npm run test:selftest
```

预期：类型检查无输出；测试全部通过（core 6 + history 4 + runner 11 = 21）。

---

## Task 12: 构建、部署与真机验收

**Files:**
- Build output: `AKA-00/static/`（`index.html`、`assets/index.js`、`.br`、`.gz`、`assets/index.css`）

- [ ] **Step 1: 构建不带屏版**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00/frontend"
WITH_SCREEN=0 npx vite build
node scripts/sri-retry.mjs
node scripts/gzip-assets.mjs
```

预期：Vite 输出 `built in ...`，`sri-retry` 打印新的 `?v=` 版本，`gzip-assets` 生成 `.br/.gz`。

- [ ] **Step 2: 本地复核产物**

```bash
grep -o '/assets/index.js?v=[a-f0-9]*' ../static/index.html
ls -l ../static/assets/index.js ../static/assets/index.js.br ../static/assets/index.js.gz ../static/assets/index.css
```

预期：index.html 里有带 `?v=` 的 js 引用；四个文件都存在且非空。

- [ ] **Step 3: 部署到小车**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车实验"
CAR_HOST=172.16.203.25 MSYS_NO_PATHCONV=1 python \
  "E:/Project/工作空间/辰龙AI教育机器人/小车实验/car-ai-senses/.debug/upload_verify.py" \
  "E:/Project/工作空间/辰龙AI教育机器人/小车代码/AKA-00/static" \
  "/root/AKA-00/static" \
  index.html assets/index.js assets/index.js.br assets/index.js.gz assets/index.css
```

预期：5 行 `ok ...`，最后 `== ok 5 bad 0`。

- [ ] **Step 4: HTTP 复核**

```bash
CAR_HOST=172.16.203.25 MSYS_NO_PATHCONV=1 python \
  "E:/Project/工作空间/辰龙AI教育机器人/小车实验/car-ai-senses/.debug/remote_exec.py" \
  "cd /root/AKA-00/static && sha256sum index.html assets/index.js assets/index.css"
```

把输出的 sha256 与本地 `sha256sum` 对比，应一致。

- [ ] **Step 5: 真机手动验收**

在手机连车的 AP（`http://192.168.4.1`）后打开 `/settings` → “自动测试”：

1. 未勾选安全确认时，开始按钮不可点。
2. 勾选后开始：预检通过，动作组按 8 行顺序执行；每对动作结束后回到原点/原朝向。
3. 每个动作行停在“待确认”，点“通过”继续；点“失败”该行变红。
4. 摄像头打开后页面出现实时画面，板载屏同步显示；关闭后画面消失。
5. 自动抓网球：页面提示放球，脚本运行，状态行更新；90s 内未完成会自动停止并记失败。
6. 勾选 WiFi：扫描到 `Tsinghua-Dongsheng`、连接成功并显示新 IP；确认操作全程通过车热点访问。
7. 勾选舵机：读取原角度 → 调整一个关节 → 恢复；结束后 `/api/arm/angles` 与测试前一致（可用 `remote_exec.py "cat /root/AKA-00/arm_angles.json"` 对比）。
8. 结束后填写备注并保存；历史列表出现该记录；点“导出 JSON”能下载文件。
9. 测试中途点“中止”：车立即停车、摄像头关闭，剩余项显示“跳过”。

- [ ] **Step 6: 记录开发日志**

```bash
cd "E:/Project/工作空间/辰龙AI教育机器人/小车实验"
node ~/.claude/session-log/session-log.mjs log "AKA-00 自动测试页 /self-test 上线：新增 src/selftest（core/history/client/runner + components）与 SelfTestPage，设置页入口；动作成对执行、摄像头/自动抓网球必测、WiFi/舵机可选，历史存 localStorage 可导出；node --test 覆盖 core/history/runner，已部署到 172.16.203.25。"
```

---

## 自检

- **规格覆盖：** 5 组测试项（动作成对、摄像头开关与显示、自动抓网球、WiFi 不恢复、舵机调整后恢复）均有对应 Task；预检、失败中止、清理、备注、历史、导出、设置入口、构建部署均已覆盖。
- **类型一致性：** `Client` 接口在 Task 4 定义，Task 5-7 的 runner 使用同一组方法名；`ItemResult`/`SelfTestRecord` 在 Task 1 定义，后续仅引用；`initialItems` 在 Task 5 定义并在页面中通过 `runSelfTest` 间接使用。
- **无占位：** Task 5 Step 4 的占位实现会在 Task 6/7 被完整替换，测试在替换前只覆盖动作组，不影响最终产物。
