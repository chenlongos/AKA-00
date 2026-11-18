// 摄像头清晰度档位（全局）
//
// 车机上是 [camera] stream_scale/width/height/quality 决定浏览器看到的画面：
// 单核所以**分辨率是帧率的主要旋钮**，quality 几乎不影响帧率（实测见 cpp/board/config.toml）。
//
// 下发有两条路，按固件能力自动选（两条都保留：老车不可能为了这个功能整包升级）：
//
//   1. 新固件：GET /api/config/camera 存在 → 直接 POST 数值。capp 在进程内改
//      config.toml + 内存里的快照，流线程下一帧就用新参数 —— **不重启、不断流**，
//      摇杆开着车也能切。
//   2. 老固件（没有上面的接口 → 404）：唯一的 root 执行通道 POST /api/ota/update，
//      下发一个 sh 脚本：车机把上传文件放到 .ota/aka-ota-update 并执行
//      `aka-ota-update --update`（先 touch 锁、kill capp）。脚本写完 config.toml 后
//      **删除 .ota/aka-ota-lock**，init.sh 的自愈循环才会重新拉起 capp（配置随之生效）。
//      脚本用 trap 保证无论成功失败都删锁——否则 HTTP 会一直挂到重启。
//
// 另外：这条老路走的 /api/ota/update 在**将来的编译版**里会清 aka-release（云端版本
// 继承记录），所以新固件有接口后务必别再走它。

// 三档：流畅 / 清晰 / 原画。
// 去掉「均衡/高清/超清」的原因：libjpeg 解码只能按 2 的幂降采样（见 image_convert.cpp），
// 出流宽 ≥320 都解 320、160–319 都解 160，所以 400/512 只是把 320 放大（最近邻，
// 出块状马赛克），288 也是 160 放大 —— 看着都一样糊，留着只会误导。
//
// 摄像头原生 320x240 直通档（4:3）试过但已移除：采集尺寸一变，demo/抓球的
// 绝对像素阈值（目标框宽 300px 等）就失真，易生歧义。
export type CameraPresetKey = "smooth" | "clear" | "native";

export type CameraPreset = {
    key: CameraPresetKey;
    label: string;
    width: number;        // 出流尺寸（浏览器看到的画面）
    height: number;
    quality: number;
    streamScale: boolean; // false = 原帧直通（零解码零编码，清晰度最高，但占用带宽）
    fpsHint: string;      // 车 A 单核实测（采集 30fps；直通受 WiFi 带宽限制）
};

// 实测（车 A，2026-11-17/18，手机热点；采集 640x360@30）：
//   256x144 q88 → 22.9fps / 1.38Mbps（解码降到 160x90，放大到 256，边缘偏块）
//   320x180 q88 → 15.3fps / 1.32Mbps（解码 1:1，重编码里最清晰）
//   640x360 原帧直通 → 14.3fps / 4.51Mbps（零解码零编码，保留全部细节）
//   （直通带宽按每路观众各算：3 路连接实测 p90 70→160ms；课堂批量用「清晰」）
// 2026-11-18 单路复测（同一台车 A，capp 单连接）：原画 26.6fps/10.5Mbps/CPU 31%。
// 直通的 CPU 几乎与分辨率无关，慢的是「解码+重编码」（清晰档 89% CPU）。
export const CAMERA_PRESETS: CameraPreset[] = [
    {key: "smooth", label: "流畅", width: 256, height: 144, quality: 88, streamScale: true,
     fpsHint: "约23帧"},
    {key: "clear", label: "清晰", width: 320, height: 180, quality: 88, streamScale: true,
     fpsHint: "约15帧"},
    {key: "native", label: "原画", width: 640, height: 360, quality: 85, streamScale: false,
     fpsHint: "约14帧·占带宽"},
];

// 没有档位记录时（老固件没写过 profile.json）按出厂配置 288x162 落「流畅」——
// 288 和 256 实际都是 160 解码放大，观感一致。
export const DEFAULT_PRESET: CameraPresetKey = "smooth";

/// 已下线的档位（均衡/高清/超清/标清）或手工改过的数值 → 归到现存三档里最接近的。
/// 直通（scale=false）一律归「原画」；缩放档按出流宽：≥640 原画、≥320 清晰、更小流畅。
const closestPreset = (scale: boolean | undefined, width: number | undefined): CameraPresetKey => {
    if (scale === false) return "native";
    if (width === undefined) return DEFAULT_PRESET;
    if (width >= 640) return "native";
    if (width >= 320) return "clear";
    return "smooth";
};

export type CameraProfile = {preset?: string; width?: number; height?: number; quality?: number};

// 车机脚本：$0 是 /root/AKA-00/.ota/aka-ota-update → APP=/root/AKA-00
// 直通档把 stream_scale 写成 false：流接口直接转发摄像头原帧（零解码零编码）。
const buildScript = (p: CameraPreset) => `#!/bin/sh
# 由控制台设置页生成的摄像头清晰度脚本（经 /api/ota/update 下发执行）
APP=$(cd "$(dirname "$0")/.." && pwd)
LOCK="$APP/.ota/aka-ota-lock"
trap 'sync; rm -f "$LOCK" "$APP/.ota/aka-ota-update" "$APP/.ota/aka-ota-install.sh" /root/aka-ota-status.json' EXIT
CFG="$APP/config.toml"
if [ -f "$CFG" ]; then
  sed -i -e 's/^stream_scale = .*/stream_scale = ${p.streamScale ? "true" : "false"}/' -e 's/^stream_width = .*/stream_width = ${p.width}/' -e 's/^stream_height = .*/stream_height = ${p.height}/' -e 's/^stream_quality = .*/stream_quality = ${p.quality}/' -e 's/^fps_streaming = .*/fps_streaming = 0/' -e 's/^fps = 15$/fps = 30/' "$CFG"
  touch /etc/aka-camera-profile-v1 /etc/aka-camera-profile-v2
  printf '{"preset":"${p.key}","width":${p.width},"height":${p.height},"quality":${p.quality},"updated":%s}\\n' "$(date +%s)" > "$APP/static/camera-profile.json"
fi
`;

export type ApplyMode = "runtime" | "restart";

/// config.toml 里的数值 → 档位 key：先精确匹配，手工改过/已下线档位则按尺寸归档
const presetKeyFor = (scale: boolean, width: number, height: number): CameraPresetKey => {
    const hit = CAMERA_PRESETS.find(p => p.streamScale === scale && p.width === width && p.height === height);
    return hit ? hit.key : closestPreset(scale, width);
};

// 能力探测只做一次（结果缓存）。车机暂时不在线（网络错误）时不缓存 —— 下次再问。
let runtimeSupport: Promise<boolean> | null = null;
const probeRuntime = (): Promise<boolean> => {
    if (!runtimeSupport) {
        runtimeSupport = fetch(`/api/config/camera?t=${Date.now()}`, {cache: "no-store"})
            .then(r => {
                if (r.ok) return true;
                if (r.status === 404) return false;   // 老固件：没有这个路由（/api/* 不走 SPA 兜底）
                throw new Error(`HTTP ${r.status}`);
            })
            .catch(e => { runtimeSupport = null; throw e; });
    }
    return runtimeSupport;
};

export const cameraProfile = {
    /// 新固件是否有运行时改档接口（老固件 → false，走 OTA 脚本 + 重启）
    supportsRuntime: (): Promise<boolean> => probeRuntime(),

    // 当前档位：新固件直接问车机（准，能反映手工改动）；老固件读脚本写的
    // static/camera-profile.json（非 /assets 路径 → no-cache）。
    current: async (): Promise<CameraProfile> => {
        try {
            const r = await fetch(`/api/config/camera?t=${Date.now()}`, {cache: "no-store"});
            if (r.ok) {
                const d = await r.json();
                const key = presetKeyFor(d.stream_scale === true, d.stream_width, d.stream_height);
                return {preset: key, width: d.stream_width, height: d.stream_height, quality: d.stream_quality};
            }
        } catch {
            // 车机不在线 → 退回文件（它也拿不到就会抛，交给调用方兜底）
        }
        // 老固件：profile.json 可能是已下线的档位（balanced/hd/sharp/eco），按宽度归档
        return fetch(`/camera-profile.json?t=${Date.now()}`, {cache: "no-store"})
            .then(r => r.json())
            .then((p: CameraProfile) => {
                const known = CAMERA_PRESETS.some(x => x.key === p.preset);
                return {...p, preset: known ? p.preset : closestPreset(undefined, p.width)};
            });
    },

    /// 下发档位。runtime = 已生效（不断流）；restart = 走 OTA 脚本，capp 约 3 秒后重启
    apply: async (p: CameraPreset): Promise<ApplyMode> => {
        if (await probeRuntime().catch(() => false)) {
            const r = await fetch("/api/config/camera", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({
                    stream_scale: p.streamScale,
                    stream_width: p.width,
                    stream_height: p.height,
                    stream_quality: p.quality,
                    preset: p.key,
                }),
            });
            const d = await r.json().catch(() => ({} as {error?: string}));
            if (!r.ok) throw new Error(d?.error || `HTTP ${r.status}`);
            return "runtime";
        }
        const fd = new FormData();
        fd.append("file", new Blob([buildScript(p)], {type: "application/octet-stream"}), "camera-profile.sh");
        const r = await fetch("/api/ota/update", {method: "POST", body: fd});
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        return "restart";
    },
};

// 等车机重启回来（下发表 → 3s 后 kill → 脚本执行 → 删锁 → init.sh 拉起 capp）
export const waitForCarBack = async (timeoutMs = 45000): Promise<boolean> => {
    await new Promise(r => setTimeout(r, 9000));
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        try {
            const r = await fetch(`/api/system/info?t=${Date.now()}`, {cache: "no-store"});
            if (r.ok) return true;
        } catch {
            // 重启中，继续等
        }
        await new Promise(r => setTimeout(r, 1500));
    }
    return false;
};
