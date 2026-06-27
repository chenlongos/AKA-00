import {useEffect, useState} from "react";
import {useViewportScale} from "../hooks/useViewportScale";

// 移动端 Fullscreen API 支持有限（iOS Safari 完全不支持 document 全屏）
const FS_API = (() => {
    const el = document.documentElement;
    return !!(
        el.requestFullscreen ||
        (el as any).webkitRequestFullscreen
    );
})();

const FullscreenButton = () => {
    const [fs, setFs] = useState(false);
    const {scalePx, scaleValue} = useViewportScale();

    useEffect(() => {
        if (!FS_API) return;
        const h = () => setFs(!!document.fullscreenElement || !!(document as any).webkitFullscreenElement);
        document.addEventListener("fullscreenchange", h);
        document.addEventListener("webkitfullscreenchange", h);
        return () => {
            document.removeEventListener("fullscreenchange", h);
            document.removeEventListener("webkitfullscreenchange", h);
        };
    }, []);

    // iOS Safari 不支持 document 全屏 API，但保留按钮（静默失败）

    const toggle = () => {
        const el = document.documentElement as any;
        if (document.fullscreenElement || (document as any).webkitFullscreenElement) {
            (document.exitFullscreen || (document as any).webkitExitFullscreen).call(document).catch(() => {});
        } else {
            (el.requestFullscreen || el.webkitRequestFullscreen).call(el).catch(() => {});
        }
    };

    const size = `${Math.max(scaleValue(32), 32)}px`;

    return (
        <button onClick={toggle} title={fs ? "退出全屏" : "全屏"} style={{
            width: size, height: size, minWidth: "32px", minHeight: "32px",
            background: fs ? "var(--color-primary-soft)" : "rgba(255,255,255,0.08)",
            border: `1.5px solid ${fs ? "var(--color-primary)" : "rgba(255,255,255,0.15)"}`,
            borderRadius: scalePx(7), color: fs ? "var(--color-primary)" : "rgba(255,255,255,0.6)",
            fontSize: scalePx(16), cursor: "pointer", display: "flex",
            alignItems: "center", justifyContent: "center",
            flexShrink: 0, outline: "none",
        }}>
            {fs ? "⛶" : "⛶"}
        </button>
    );
};

export default FullscreenButton;
