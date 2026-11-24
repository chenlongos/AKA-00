import test from "node:test";
import assert from "node:assert/strict";
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

test("planArmSweep: zp10s 三个舵机大幅摆动（底座/腕部/夹爪）", () => {
  const angles = {
    grab_position: {servo0: 228, servo1: 86},
    lift_position: {servo0: 151, servo1: 91},
    gripper_open: 135,
    gripper_close: 45,
  };
  const plan = planArmSweep("zp10s", angles);
  assert.deepEqual(plan.moves.map(move => move.key), [
    "grab_position.servo0", "grab_position.servo1", "gripper_open",
  ]);
  assert.deepEqual(plan.moves.map(move => move.label), ["底座旋转", "腕部", "夹爪"]);
  assert.deepEqual(plan.moves.map(move => move.to), [188, 126, 45]);
  // 只做预览，不改原始配置
  assert.deepEqual(plan.original, angles);
});

test("planArmSweep: sts3215 用 servo1/servo2，步幅 400", () => {
  const plan = planArmSweep("sts3215", {
    grab_position: {servo1: 1850, servo2: 2650},
    lift_position: {servo1: 2300, servo2: 2100},
    gripper_open: 4000,
    gripper_close: 3000,
  });
  assert.deepEqual(plan.moves.map(move => move.key), [
    "grab_position.servo1", "grab_position.servo2", "gripper_open",
  ]);
  assert.deepEqual(plan.moves.map(move => move.to), [2250, 2250, 3000]);
});

test("planArmSweep: 缺参数时抛错", () => {
  assert.throws(() => planArmSweep("zp10s", {}), /没有可调整/);
});

test("normalizeDemoParams: 默认值与 Demo 页一致，越界收敛、非数字回落", () => {
  assert.deepEqual(DEMO_DEFAULTS, {target_size: 300, speed: 25, turn_speed: 25});
  assert.deepEqual(normalizeDemoParams({}), DEMO_DEFAULTS);
  assert.deepEqual(
    normalizeDemoParams({target_size: 9999, speed: 0, turn_speed: 120}),
    {target_size: 1000, speed: 5, turn_speed: 60},
  );
  assert.deepEqual(
    normalizeDemoParams({target_size: 280, speed: Number.NaN, turn_speed: 22.6}),
    {target_size: 280, speed: 25, turn_speed: 23},
  );
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

test("UNATTENDED_GROUPS: 覆盖全部五组且 WiFi 最后", () => {
  assert.deepEqual(UNATTENDED_GROUPS, ["motion", "camera", "demo", "arm", "wifi"]);
});
