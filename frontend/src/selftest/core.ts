// 自动测试的纯逻辑：不依赖 DOM / fetch，可用 node --test 直接跑。
// 注意：Node 的 --experimental-strip-types 只支持可擦除语法（无 enum / namespace）。

export type Group = "motion" | "camera" | "demo" | "wifi" | "arm";
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
  /** 本次自动抓网球用的参数（旧记录可能没有） */
  demo?: DemoParams;
  items: ItemResult[];
  summary: SelfTestSummary;
  remark: string;
  /** 手动逐项确认 / 全自动无人值守（旧记录缺省视为手动） */
  mode?: "manual" | "unattended";
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

export interface ArmMove {
  /** preview 接口的 key，如 "grab_position.servo0" / "gripper_open" */
  key: string;
  /** 中文名：底座旋转 / 腕部 / 夹爪 */
  label: string;
  from: number;
  to: number;
}

export interface ArmSweepPlan {
  driver: string;
  original: Record<string, unknown>;
  moves: ArmMove[];
}

/** 每种驱动的两个手臂舵机 + 摆动步幅（幅度要大到人眼能明显看出动作） */
const ARM_SWEEP: Record<string, {servos: string[]; step: number; min: number; max: number}> = {
  zp10s: {servos: ["servo0", "servo1"], step: 40, min: 0, max: 270},
  sts3215: {servos: ["servo1", "servo2"], step: 400, min: 0, max: 4095},
};

function numberAt(node: unknown, ...path: string[]): number | null {
  let cursor: unknown = node;
  for (const key of path) {
    if (!cursor || typeof cursor !== "object") return null;
    cursor = (cursor as Record<string, unknown>)[key];
  }
  return typeof cursor === "number" && Number.isFinite(cursor) ? cursor : null;
}

/** 向量程中心方向摆一步，离开两端机械限位 */
function swingTo(from: number, step: number, min: number, max: number): number {
  const center = (min + max) / 2;
  const preferred = from > center ? from - step : from + step;
  const clamped = Math.min(max, Math.max(min, preferred));
  if (clamped !== from) return clamped;
  const reverse = from > center ? from + step : from - step;
  return Math.min(max, Math.max(min, reverse));
}

/**
 * 舵机故障排查计划：三个舵机轮流大幅摆一次——
 * 底座旋转 / 腕部（grab_position 里的两个舵机）+ 夹爪（张开 → 闭合）。
 * 只做角度预览（preview），不改写配置；恢复由 runner 逆序写回原始值。
 */
export function planArmSweep(driver: string, angles: Record<string, unknown>): ArmSweepPlan {
  const conf = ARM_SWEEP[driver] ?? ARM_SWEEP.zp10s;
  const original = JSON.parse(JSON.stringify(angles)) as Record<string, unknown>;
  const moves: ArmMove[] = [];

  conf.servos.forEach((servo, index) => {
    const value = numberAt(angles, "grab_position", servo);
    if (value === null) return;
    moves.push({
      key: `grab_position.${servo}`,
      label: index === 0 ? "底座旋转" : "腕部",
      from: value,
      to: swingTo(value, conf.step, conf.min, conf.max),
    });
  });

  const open = numberAt(angles, "gripper_open");
  const close = numberAt(angles, "gripper_close");
  if (open !== null && close !== null && open !== close) {
    moves.push({key: "gripper_open", label: "夹爪", from: open, to: close});
  }

  if (moves.length === 0) throw new Error("角度配置里没有可调整的舵机参数");
  return {driver, original, moves};
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

export interface DemoParams {
  /** 目标框宽（原图像素），框宽达到它就认为到位 */
  target_size: number;
  /** 直线速度 % */
  speed: number;
  /** 转弯速度 % */
  turn_speed: number;
}

/** 默认值与「自动抓网球」Demo 页的 DEFAULT_PARAMS 保持一致 */
export const DEMO_DEFAULTS: DemoParams = {target_size: 300, speed: 25, turn_speed: 25};

const DEMO_LIMITS: Record<keyof DemoParams, {min: number; max: number}> = {
  target_size: {min: 50, max: 1000},
  speed: {min: 5, max: 70},
  turn_speed: {min: 5, max: 60},
};

/** 把界面输入收敛到脚本/固件能接受的范围；非数字回落到默认值 */
export function normalizeDemoParams(input: Partial<DemoParams>): DemoParams {
  const pick = (key: keyof DemoParams) => {
    const raw = input[key];
    const value = typeof raw === "number" && Number.isFinite(raw) ? Math.round(raw) : DEMO_DEFAULTS[key];
    const {min, max} = DEMO_LIMITS[key];
    return Math.min(max, Math.max(min, value));
  };
  return {
    target_size: pick("target_size"),
    speed: pick("speed"),
    turn_speed: pick("turn_speed"),
  };
}
