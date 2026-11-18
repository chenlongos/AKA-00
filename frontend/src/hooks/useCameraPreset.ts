import {useCallback, useEffect, useRef, useState} from "react";
import {
    CAMERA_PRESETS, DEFAULT_PRESET, cameraProfile, waitForCarBack,
    type ApplyMode, type CameraPresetKey,
} from "../cameraProfile";

// 全局摄像头清晰度档位：设置页与摇杆页共用同一份状态与下发逻辑。
// 新固件（有 POST /api/config/camera）→ 就地生效，不重启、不断流；
// 老固件 → 写 config.toml 后 capp 重启（约 10 秒），期间摄像头/摇杆 WS 都会短暂断开。
export const useCameraPreset = () => {
    const [current, setCurrent] = useState<CameraPresetKey | null>(null);
    const [sel, setSel] = useState<CameraPresetKey>(DEFAULT_PRESET);
    const [busy, setBusy] = useState(false);
    const [msg, setMsg] = useState("");
    const flashTimer = useRef<number | null>(null);

    const flash = useCallback((text: string, holdMs = 0) => {
        if (flashTimer.current !== null) { window.clearTimeout(flashTimer.current); flashTimer.current = null; }
        setMsg(text);
        if (holdMs > 0) {
            flashTimer.current = window.setTimeout(() => { setMsg(""); flashTimer.current = null; }, holdMs);
        }
    }, []);

    useEffect(() => () => { if (flashTimer.current !== null) window.clearTimeout(flashTimer.current); }, []);

    useEffect(() => {
        cameraProfile.current()
            .then(p => {
                // 只认已知档位（手工改过 config 时文件里可能是别的值，下拉框不能拿它当 value）
                const known = CAMERA_PRESETS.some(x => x.key === p.preset) ? p.preset as CameraPresetKey : DEFAULT_PRESET;
                setCurrent(known);
                setSel(known);
            })
            .catch(() => { /* 还没写过该文件：保持默认「均衡」，下拉仍可选择 */ });
    }, []);

    /**
     * 下发档位。返回实际走的通道（runtime = 已就地生效；restart = capp 重启过），
     * 失败返回 null。
     * onBefore 只在**老固件（要重启）**时调用 —— 摇杆页用它先停车（重启期间 WS 会断）；
     * 新固件不断流，开着车切换也不会失控，不必停车。
     */
    const apply = useCallback(async (onBefore?: () => void): Promise<ApplyMode | null> => {
        if (busy) return null;
        const preset = CAMERA_PRESETS.find(p => p.key === sel);
        if (!preset) return null;
        setBusy(true);
        const runtime = await cameraProfile.supportsRuntime().catch(() => false);
        if (runtime) {
            flash("正在切换清晰度…");
        } else {
            onBefore?.();
            flash("正在应用，车机会重启摄像头服务（约 10 秒）…");
        }
        try {
            const mode = await cameraProfile.apply(preset);
            if (mode === "restart") {
                if (!await waitForCarBack()) {
                    flash("车机暂未响应，请稍后刷新页面确认");
                    return null;
                }
            }
            setCurrent(preset.key);
            flash(`已切换为「${preset.label}」（${preset.width}×${preset.height}）`, 5000);
            return mode;
        } catch {
            flash("下发失败，请重试");
            return null;
        } finally {
            setBusy(false);
        }
    }, [busy, sel, flash]);

    return {current, sel, setSel, busy, msg, apply};
};
