import {CAMERA_PRESETS, type CameraPresetKey} from "../cameraProfile";
import {useCameraPreset} from "../hooks/useCameraPreset";
import {useViewportScale} from "../hooks/useViewportScale";

interface CameraQualityPickerProps {
    /** 只在老固件（要重启 capp、WS 会断）时调用 —— 摇杆页用来先停车 */
    onBeforeApply?: () => void;
    /** 只在老固件重启后调用（摇杆页用来重新发起 MJPEG 请求；新固件不断流，不用重发） */
    onApplied?: () => void;
}

// 全局清晰度下拉（叠在视频/深色底上的紧凑样式）。
// 选档不立即下发：改了下拉后右侧才出现「应用」，避免误触。
const CameraQualityPicker = ({onBeforeApply, onApplied}: CameraQualityPickerProps) => {
    const {scalePx} = useViewportScale();
    const {current, sel, setSel, busy, msg, apply} = useCameraPreset();
    const pending = current !== null && sel !== current;

    const handleApply = async () => {
        const mode = await apply(onBeforeApply);
        if (mode === "restart") onApplied?.();
    };

    return (
        <>
            <div style={{display: "flex", alignItems: "center", gap: scalePx(4)}}>
                <select
                    value={sel}
                    disabled={busy}
                    onChange={e => setSel(e.target.value as CameraPresetKey)}
                    style={{
                        maxWidth: scalePx(96), padding: `${scalePx(3)} ${scalePx(4)}`,
                        borderRadius: scalePx(5), border: "1px solid rgba(255,255,255,0.25)",
                        background: "rgba(0,0,0,0.55)", color: "#fff",
                        fontSize: scalePx(10), fontWeight: 600, outline: "none",
                        opacity: busy ? 0.6 : 1,
                    }}
                >
                    {CAMERA_PRESETS.map(p => (
                        <option key={p.key} value={p.key} style={{color: "#000"}}>
                            {p.label} {p.width}×{p.height}
                        </option>
                    ))}
                </select>
                {(pending || busy) && (
                    <button
                        onClick={handleApply}
                        disabled={busy}
                        style={{
                            padding: `${scalePx(3)} ${scalePx(8)}`, border: "none", borderRadius: scalePx(5),
                            background: busy ? "rgba(255,255,255,0.2)" : "var(--color-success)",
                            color: "#fff", fontSize: scalePx(10), fontWeight: 700,
                            cursor: busy ? "wait" : "pointer", outline: "none", whiteSpace: "nowrap",
                        }}
                    >
                        {busy ? "应用中…" : "应用"}
                    </button>
                )}
            </div>
            {msg && (
                <div style={{
                    position: "fixed", top: scalePx(52), left: 0, right: 0, zIndex: 99,
                    display: "flex", justifyContent: "center", pointerEvents: "none",
                }}>
                    <span style={{
                        padding: `${scalePx(5)} ${scalePx(12)}`, borderRadius: scalePx(14),
                        background: "rgba(0,0,0,0.72)", color: "#fff",
                        fontSize: scalePx(11), fontWeight: 600, whiteSpace: "nowrap",
                    }}>{msg}</span>
                </div>
            )}
        </>
    );
};

export default CameraQualityPicker;
