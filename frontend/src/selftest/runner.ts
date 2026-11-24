import {
  DEMO_TIMEOUT_MS,
  MOTION_ROWS,
  WIFI_SSID,
  planArmSweep,
  sameAngles,
} from "./core.ts";
import type { ArmSweepPlan, Cmd, DemoParams, Group, ItemResult, ItemStatus, RunState, StepResult } from "./core.ts";
import type { Client } from "./client.ts";

export interface ArmSnapshot {
  driver: string;
  angles: Record<string, unknown>;
}

export interface RunnerOptions {
  groups: Group[];
  speed: number;
  /** 自动抓网球脚本参数（直行 / 转弯速度、目标框宽） */
  demoParams: DemoParams;
  abortOnFailure: boolean;
  /** 全自动（无人值守）：不弹确认/提示、动作失败不连锁跳过，判定只看接口结果 */
  unattended?: boolean;
  client: Client;
  cameraFrame: (timeoutMs: number) => Promise<boolean>;
  confirm: (itemId: string, message: string) => Promise<boolean>;
  /** 单项执行前的阻塞提示（如自动抓网球前提醒放球）；返回 false 表示跳过该项 */
  notice?: (itemId: string, message: string) => Promise<boolean>;
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

/** 单项执行只需要这些选项；整套流程与单项重测共用同一套 helpers */
type ItemCtx = Omit<RunnerOptions, "groups" | "onUpdate">;

export interface RetestOptions extends ItemCtx {
  /** 单项状态变化（含中间态），供页面原地更新那一行 */
  onItem: (item: ItemResult) => void;
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
  {id: "arm_roundtrip", name: "舵机角度 调整 / 恢复", group: "arm"},
  {id: "wifi_scan", name: "WiFi 扫描", group: "wifi"},
  {id: "wifi_connect", name: `WiFi 连接 ${WIFI_SSID}`, group: "wifi"},
  {id: "wifi_verify", name: "WiFi 验证", group: "wifi"},
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
        if (!passed && opts.abortOnFailure && !opts.unattended) skipRest = true;
      }
    }

    if (!aborted && !checkAbort() && opts.groups.includes("camera")) {
      await runCamera(opts, patch, now);
    }

    if (!aborted && !checkAbort() && opts.groups.includes("demo")) {
      await runDemo(opts, patch, now, hasTennis);
    }

    if (!aborted && !checkAbort() && opts.groups.includes("arm")) {
      arm = await runArm(opts, patch, now);
    }

    if (!aborted && !checkAbort() && opts.groups.includes("wifi")) {
      await runWifi(opts, patch, now);
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
  opts: ItemCtx,
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
  if (auto === "pass" && !opts.unattended) {
    patch(id, {status: "confirm", auto});
    manual = await opts.confirm(id, "动作是否到位？（距离 / 角度）") ? "pass" : "fail";
    status = manual;
  }
  patch(id, {status, auto, manual, elapsedMs: now() - started});
  return status === "pass";
}

async function runGrab(
  id: string,
  opts: ItemCtx,
  patch: Patch,
  now: () => number,
): Promise<boolean> {
  const started = now();
  const steps: StepResult[] = [];
  patch(id, {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});

  const grab = await opts.client.control("grab", {speed: opts.speed, timeoutMs: 10000});
  steps.push({name: "抓取", ok: grab.ok, elapsedMs: grab.elapsedMs, error: grab.error});
  patch(id, {steps: steps.map(step => ({...step})), elapsedMs: now() - started});

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
}

async function openCameraItem(opts: ItemCtx, patch: Patch, now: () => number): Promise<void> {
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
    patch("camera_on", {
      status: "fail",
      auto: "fail",
      steps,
      elapsedMs: now() - started,
      error: open.error ?? "未收到画面",
    });
  }
}

async function closeCameraItem(opts: ItemCtx, patch: Patch, now: () => number): Promise<void> {
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
  opts.onCamera(false);  // 摄像头关闭后立刻撤掉行内画面
}

async function runCamera(opts: ItemCtx, patch: Patch, now: () => number): Promise<void> {
  await openCameraItem(opts, patch, now);
  await closeCameraItem(opts, patch, now);
}

async function runDemo(
  opts: ItemCtx,
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

  const placed = opts.unattended
    ? true
    : (await opts.notice?.(id, "请把网球放在小车正前方 30–60cm 处，并移开周围障碍物")) ?? true;
  if (!placed) {
    patch(id, {status: "skip", error: "未放置网球，已跳过", elapsedMs: now() - started});
    return;
  }

  const start = await opts.client.demoStart(opts.demoParams);
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
    // 只有这三个是终态；"idle" 可能出现在脚本线程启动前，继续等。
    if (["done", "failed", "aborted"].includes(state)) break;
    await opts.client.sleep(1000);
  }

  if (opts.isAborted()) {
    await opts.client.demoStop().catch(() => {});
    patch(id, {status: "skip", error: "已中止", elapsedMs: now() - started});
    return;
  }

  const timedOut = now() >= deadline || !["done", "failed", "aborted"].includes(state);
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
}

async function scanWifi(opts: ItemCtx, patch: Patch, now: () => number): Promise<boolean> {
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
  return scanOk;
}

async function connectWifi(opts: ItemCtx, patch: Patch, now: () => number): Promise<boolean> {
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
  return connectOk;
}

async function verifyWifi(opts: ItemCtx, patch: Patch, now: () => number): Promise<void> {
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

async function runWifi(opts: ItemCtx, patch: Patch, now: () => number): Promise<void> {
  await scanWifi(opts, patch, now);
  const connectOk = await connectWifi(opts, patch, now);
  if (!connectOk) {
    patch("wifi_verify", {status: "skip", error: "连接失败，未验证"});
    return;
  }
  await verifyWifi(opts, patch, now);
}

async function runArm(opts: ItemCtx, patch: Patch, now: () => number): Promise<ArmSnapshot | null> {
  const id = "arm_roundtrip";
  const started = now();
  const steps: StepResult[] = [];
  const flush = () => patch(id, {steps: steps.map(step => ({...step}))});
  patch(id, {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});

  const read = await opts.client.armRead();
  if (!read.ok) {
    patch(id, {status: "fail", auto: "fail", error: read.error ?? "读取角度失败", elapsedMs: now() - started});
    return null;
  }

  const original: ArmSnapshot = {driver: read.driver, angles: read.angles};
  opts.onArmSnapshot(original);
  steps.push({name: "读取原始角度", ok: true, elapsedMs: 0, detail: read.driver});
  flush();

  const allowed = opts.unattended
    ? true
    : await opts.confirm(id, "确认机械臂周围无障碍，开始逐个转动底座 / 腕部 / 夹爪？");
  if (!allowed) {
    patch(id, {status: "skip", steps: steps.map(step => ({...step})), error: "操作员取消"});
    return original;
  }

  let plan: ArmSweepPlan;
  try {
    plan = planArmSweep(read.driver, original.angles);
  } catch (e) {
    patch(id, {
      status: "fail",
      auto: "fail",
      steps: steps.map(step => ({...step})),
      error: e instanceof Error ? e.message : String(e),
      elapsedMs: now() - started,
    });
    return original;
  }

  // 三个舵机依次大幅摆动：preview 直接把该舵机转过去，不写入测试值
  let movesOk = true;
  for (const move of plan.moves) {
    const result = await opts.client.armPreview(read.driver, move.key, move.to, original.angles);
    movesOk = movesOk && result.ok;
    steps.push({
      name: `${move.label} ${move.from} → ${move.to}`,
      ok: result.ok,
      elapsedMs: 0,
      error: result.error,
    });
    flush();
    await opts.client.sleep(1000);
  }

  let observed = true;
  if (!opts.unattended) {
    patch(id, {status: "confirm"});
    observed = await opts.confirm(id, "底座 / 腕部 / 夹爪是否都有动作？");
    steps.push({name: "人工确认三个舵机动作", ok: observed, elapsedMs: 0});
    flush();
  }

  // 逆序恢复：夹爪 → 腕部 → 底座，最后回到初始姿态
  let restored = true;
  for (const move of [...plan.moves].reverse()) {
    const result = await opts.client.armPreview(read.driver, move.key, move.from, original.angles);
    restored = restored && result.ok;
    steps.push({name: `${move.label} 恢复 ${move.to} → ${move.from}`, ok: result.ok, elapsedMs: 0, error: result.error});
    flush();
    await opts.client.sleep(800);
  }

  // 恢复后再确认角度参数与测试前完全一致
  const verify = await opts.client.armRead();
  const configOk = verify.ok && sameAngles(verify.angles, original.angles);
  steps.push({name: "恢复后参数与原始值一致", ok: configOk, elapsedMs: 0});
  flush();

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
}

/** 单项重测：整套流程跑完后，操作员可以只重跑某一项（其他项结果不动） */
export async function retestItem(itemId: string, opts: RetestOptions): Promise<ItemResult> {
  const now = opts.now ?? (() => Date.now());
  const items = initialItems([]);
  const target = items.find(item => item.id === itemId);
  if (!target) throw new Error(`未知测试项：${itemId}`);

  const copy = (): ItemResult => ({...target, steps: target.steps.map(step => ({...step}))});
  const patch: Patch = (id, change) => {
    if (id !== itemId) return;  // 摄像头 / WiFi 的联动项只在整套流程里更新
    Object.assign(target, change);
    opts.onItem(copy());
  };

  patch(itemId, {status: "running", steps: [], auto: null, manual: null, error: undefined, elapsedMs: 0});

  let arm: ArmSnapshot | null = null;
  try {
    const row = MOTION_ROWS.find(entry => entry.id === itemId);
    if (row) {
      if (row.kind === "grab") await runGrab(itemId, opts, patch, now);
      else await runPair(itemId, row.first, row.second, opts, patch, now);
    } else if (itemId === "camera_on") {
      await openCameraItem(opts, patch, now);
    } else if (itemId === "camera_off") {
      await closeCameraItem(opts, patch, now);
    } else if (itemId === "demo_grab") {
      await runDemo(opts, patch, now, await opts.client.demoHasTennis());
    } else if (itemId === "wifi_scan") {
      await scanWifi(opts, patch, now);
    } else if (itemId === "wifi_connect") {
      await connectWifi(opts, patch, now);
    } else if (itemId === "wifi_verify") {
      await verifyWifi(opts, patch, now);
    } else if (itemId === "arm_roundtrip") {
      arm = await runArm(opts, patch, now);
    }
  } finally {
    await opts.client.stop().catch(() => {});
    await opts.client.cameraClose().catch(() => {});
    opts.onCamera(false);
    if (arm) await opts.client.armWrite(arm.driver, arm.angles).catch(() => {});
  }
  return copy();
}
