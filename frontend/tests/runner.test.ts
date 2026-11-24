import test from "node:test";
import assert from "node:assert/strict";
import {retestItem, runSelfTest} from "../src/selftest/runner.ts";
import type { Client } from "../src/selftest/client.ts";
import type { DemoParams, Group, ItemResult } from "../src/selftest/core.ts";

interface Fake {
  client: Client;
  calls: string[];
  demoStarts: DemoParams[];
}

const ARM_ANGLES = () => ({
  grab_position: {servo0: 228, servo1: 86},
  lift_position: {servo0: 151, servo1: 91},
  gripper_open: 135,
  gripper_close: 45,
});

function fakeClient(): Fake {
  const calls: string[] = [];
  const demoStarts: DemoParams[] = [];
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
    demoStart: async (params) => {
      calls.push("demo.start");
      demoStarts.push(structuredClone(params));
      return {ok: true};
    },
    demoStatus: async () => ({ok: true, state: "done"}),
    demoStop: async () => { calls.push("demo.stop"); },
    demoHasTennis: async () => true,
    wifiScan: async () => ({ok: true, list: [{ssid: "Tsinghua-Dongsheng", signal: -40, secured: false}]}),
    wifiConnect: async () => ({ok: true, ip: "192.168.9.9"}),
    wifiStatus: async () => ({ok: true, ssid: "Tsinghua-Dongsheng", ip: "192.168.9.9"}),
    armRead: async () => ({ok: true, driver: "zp10s", angles: ARM_ANGLES()}),
    armWrite: async () => ({ok: true}),
    armPreview: async () => ({ok: true}),
    motorConnected: async () => true,
    systemInfo: async () => ({mac: "AA:BB:CC:DD:EE:FF", ip: "1.2.3.4"}),
    version: async () => "v1",
    sleep: async () => {},
  };
  return {client, calls, demoStarts};
}

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
  assert.deepEqual(fake.calls.filter(call => call.startsWith("camera.") || call === "stop"), ["stop", "camera.close"]);
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
  let controlCalls = 0;
  const {fake, outcome} = await run(["motion"], {
    control: async () => {
      controlCalls += 1;
      return controlCalls === 1
        ? {ok: false, completed: false, error: "动作失败", elapsedMs: 1}
        : {ok: true, completed: true, elapsedMs: 1};
    },
  });
  const items = motionItems(outcome.items);
  assert.equal(items[0].status, "fail");
  assert.equal(items.slice(1).every(item => item.status === "skip"), true);
  assert.equal(fake.calls.includes("stop"), true);
});

test("摄像头组：打开→首帧→确认→关闭并校验", async () => {
  const {fake, outcome} = await run(["camera"]);
  assert.deepEqual(fake.calls.filter(call => call.startsWith("camera.")), ["camera.open", "camera.close", "camera.close"]);
  assert.equal(fake.calls.includes("stop"), true);
  const items = outcome.items.filter(item => item.group === "camera");
  assert.deepEqual(items.map(item => item.status), ["pass", "pass"]);
});

test("自动抓网球：状态 done 且人工确认后通过", async () => {
  const {fake, outcome} = await run(["demo"]);
  assert.equal(fake.calls.includes("demo.start"), true);
  assert.deepEqual(fake.demoStarts, [{target_size: 280, speed: 30, turn_speed: 22}]);
  const item = outcome.items.find(item => item.id === "demo_grab")!;
  assert.equal(item.status, "pass");
  assert.equal(fake.calls.includes("demo.stop"), false);
});

test("自动抓网球：开始前弹出放球提示，取消则跳过且不启动 demo", async () => {
  const notices: string[] = [];
  const {fake, outcome} = await run(["demo"], {}, {
    notice: (id) => { notices.push(id); return false; },
  });
  const item = outcome.items.find(entry => entry.id === "demo_grab")!;
  assert.equal(item.status, "skip");
  assert.deepEqual(notices, ["demo_grab"]);
  assert.equal(fake.calls.includes("demo.start"), false);
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

test("舵机组：三个舵机逐个大幅摆动，人工确认后逆序恢复", async () => {
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
  assert.deepEqual(previews, [
    {key: "grab_position.servo0", value: 188},
    {key: "grab_position.servo1", value: 126},
    {key: "gripper_open", value: 45},
    {key: "gripper_open", value: 135},
    {key: "grab_position.servo1", value: 86},
    {key: "grab_position.servo0", value: 228},
  ]);
  assert.equal(confirms, 2);  // 开始确认 + 人工确认动作
  assert.equal(writes.length, 1);  // finally 写回原始配置
  assert.deepEqual(writes[0], ARM_ANGLES());
});

test("舵机组：某个舵机预览失败仍会恢复并记失败", async () => {
  const previews: Array<{key: string; value: number}> = [];
  let write: Record<string, unknown> | null = null;
  const {outcome} = await run(["arm"], {
    armPreview: async (_driver, key, value) => {
      previews.push({key, value});
      return key === "grab_position.servo0" && value === 188
        ? {ok: false, error: "串口无响应"}
        : {ok: true};
    },
    armWrite: async (_driver, next) => {
      write = structuredClone(next);
      return {ok: true};
    },
  }, {
    confirm: () => true,
  });
  const item = outcome.items.find(entry => entry.id === "arm_roundtrip")!;
  assert.equal(item.status, "fail");
  assert.equal(previews[previews.length - 1].value, 228);  // 最后仍恢复到原始值
  assert.deepEqual(write, ARM_ANGLES());
});

test("中止：未开始的项全部跳过，清理命令仍执行", async () => {
  const {fake, outcome} = await run(["motion", "camera"], {}, {isAborted: () => true});
  assert.equal(outcome.state, "aborted");
  assert.equal(outcome.items.every(item => item.status === "skip"), true);
  assert.equal(fake.calls.includes("stop"), true);
  assert.equal(fake.calls.includes("camera.close"), true);
});

async function retest(id: string, overrides: Partial<Client> = {}, options: {
  confirm?: (id: string) => boolean;
  demoParams?: DemoParams;
} = {}) {
  const fake = fakeClient();
  Object.assign(fake.client, overrides);
  const updates: ItemResult[] = [];
  const item = await retestItem(id, {
    speed: 25,
    demoParams: options.demoParams ?? {target_size: 280, speed: 30, turn_speed: 22},
    abortOnFailure: true,
    client: fake.client,
    cameraFrame: async () => true,
    confirm: async (itemId) => options.confirm ? options.confirm(itemId) : true,
    notice: async () => true,
    isAborted: () => false,
    onItem: next => updates.push(next),
    onArmSnapshot: () => {},
    onCamera: () => {},
  });
  return {fake, item, updates};
}

test("单项重测：只执行该项动作，回调只更新这一项", async () => {
  const {fake, item, updates} = await retest("turn_30");
  assert.equal(item.status, "pass");
  assert.deepEqual(fake.calls, ["left@30deg", "right@30deg", "stop", "camera.close"]);
  assert.equal(updates.length > 0, true);
  assert.equal(updates.every(entry => entry.id === "turn_30"), true);
});

test("单项重测：自动抓网球使用页面传入的参数", async () => {
  const {fake, item} = await retest("demo_grab", {}, {
    demoParams: {target_size: 280, speed: 24, turn_speed: 25},
  });
  assert.deepEqual(fake.demoStarts, [{target_size: 280, speed: 24, turn_speed: 25}]);
  assert.equal(item.status, "pass");
});

test("单项重测：摄像头关闭项只关不开", async () => {
  const {fake, item} = await retest("camera_off");
  assert.equal(fake.calls.includes("camera.open"), false);
  assert.equal(fake.calls.filter(call => call === "camera.close").length, 2);  // 项内 + finally
  assert.equal(item.status, "pass");
});

test("单项重测：未知测试项直接报错", async () => {
  await assert.rejects(retest("nope"), /未知测试项/);
});

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
