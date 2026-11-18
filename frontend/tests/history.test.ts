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
