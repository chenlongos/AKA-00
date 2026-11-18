import { buildControlUrl, classifyControl, normalizeDemoParams } from "./core.ts";
import type { ControlOptions, DemoParams } from "./core.ts";

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
  demoStart(params: DemoParams): Promise<SimpleOutcome>;
  demoStatus(): Promise<DemoStatusOutcome>;
  demoStop(): Promise<void>;
  demoHasTennis(): Promise<boolean>;
  wifiScan(): Promise<WifiScanOutcome>;
  wifiConnect(ssid: string, password: string): Promise<WifiConnectOutcome>;
  wifiStatus(): Promise<WifiStatusOutcome>;
  armRead(): Promise<ArmReadOutcome>;
  armWrite(driver: string, angles: Record<string, unknown>): Promise<SimpleOutcome>;
  /** 立即把某个舵机转到指定角度（角度配置原样保存，不写入测试值） */
  armPreview(driver: string, key: string, value: number, angles: Record<string, unknown>): Promise<SimpleOutcome>;
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

    demoStart: async (params) => {
      const result = await requestJson("/api/demo/init", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({
          action: "grab",
          model: "tennis",
          mode: "once",
          ...normalizeDemoParams(params),
          wait: false,
        }),
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

    armPreview: async (driver, key, value, angles) => {
      const result = await requestJson("/api/arm/angles/preview", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({driver, key, value, angles}),
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
