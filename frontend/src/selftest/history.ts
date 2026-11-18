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
