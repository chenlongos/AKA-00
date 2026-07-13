import {useEffect, useRef, useState} from "react";
import {api} from "../api";
import {useViewportScale} from "../hooks/useViewportScale";

interface CameraToggleProps {
    onStatusChange?: (on: boolean) => void;
    /**
     * 首帧观测信号。
     * - undefined: 这个 toggle 不接 MJPEG 渲染（BaseControlPage 等），loading 跟 HTTP 同步即关
     * - false:     上层在等首帧，loading 暂留
     * - true:      首帧已到，清 loading
     *
     * 实现理由：camera-node 从 `cam.open()` 到 JPEG 第一帧推到 web-server 的 `<img>`，
     * host 上 ~100-200ms，板上更慢。期间用户点"打开"看到的是空白，
     * 单纯 HTTP 同步会过早结束 loading。RCDemoPage 应传 frameReady=false→true 的状态机。
     */
    frameReady?: boolean;
}

const spinKeyframes = `
@keyframes camera-toggle-spin {
    to { transform: rotate(360deg); }
}`;
let _styleInjected = false;

const CameraToggle = ({onStatusChange, frameReady}: CameraToggleProps) => {
    const [on, setOn] = useState(false);
    const [loading, setLoading] = useState(false);
    const {scalePx} = useViewportScale();
    const mountedRef = useRef(true);

    useEffect(() => {
        if (!_styleInjected) {
            _styleInjected = true;
            const style = document.createElement("style");
            style.textContent = spinKeyframes;
            document.head.appendChild(style);
        }
    }, []);

    useEffect(() => {
        mountedRef.current = true;
        api.camera.status().then(data => {
            if (!mountedRef.current) return;
            setOn(data.camera_on);
            onStatusChange?.(data.camera_on);
        }).catch(() => {});
        return () => { mountedRef.current = false; };
    }, []);

    const toggle = () => {
        if (loading) return;
        setLoading(true);
        const req = on ? api.camera.close() : api.camera.open();
        req.then(data => {
            if (!mountedRef.current) return;
            const newOn = data.camera_on;
            setOn(newOn);
            onStatusChange?.(newOn);
            // 关闭：HTTP 同步就是终态，立刻清 loading
            // 打开：无 frameReady 跟踪（无 MJPEG 视图）→ 同样立刻清
            // 打开 + 有 frameReady → 暂留，等上层 img.onLoad 把 frameReady 拉到 true
            if (!newOn || frameReady === undefined) {
                setLoading(false);
            }
        }).catch(() => {
            // 网络错误 / 后端报错 → 别死锁，清 loading
            if (mountedRef.current) setLoading(false);
        });
    };

    // 上层告诉我们"首帧到了"——这是关 loading 的最终信号。
    useEffect(() => {
        if (frameReady === true && loading) {
            setLoading(false);
        }
    }, [frameReady, loading]);

    // 兜底安全网：万一上层忘记传 frameReady / onLoad 永远不 fire / props 抖动，
    // 3s 内强制清 loading，绝不卡死 spinner。
    useEffect(() => {
        if (!loading) return;
        const t = window.setTimeout(() => {
            if (mountedRef.current) setLoading(false);
        }, 3000);
        return () => window.clearTimeout(t);
    }, [loading]);

    return (
        <div
            onClick={toggle}
            style={{
                width: scalePx(44),
                height: scalePx(24),
                borderRadius: scalePx(12),
                background: on ? "var(--color-success)" : "var(--color-bg-elevated)",
                cursor: loading ? "wait" : "pointer",
                opacity: loading ? 0.6 : 1,
                position: "relative",
                transition: "background 0.2s, opacity 0.15s",
                flexShrink: 0,
            }}
        >
            <div style={{
                position: "absolute",
                top: "50%",
                transform: "translateY(-50%)",
                left: on ? scalePx(24) : scalePx(3),
                width: scalePx(18),
                height: scalePx(18),
                borderRadius: "50%",
                background: loading ? "#ddd" : "white",
                transition: "left 0.2s ease, background 0.15s",
                boxShadow: loading ? "none" : "0 1px 3px rgba(0,0,0,0.3)",
                display: "flex", alignItems: "center", justifyContent: "center",
            }}>
                {loading && (
                    <span style={{
                        width: scalePx(10), height: scalePx(10),
                        borderRadius: "50%",
                        border: `2px solid #999`,
                        borderTopColor: "transparent",
                        animation: "camera-toggle-spin 0.6s linear infinite",
                        display: "block",
                    }}/>
                )}
            </div>
        </div>
    );
};

export default CameraToggle;
