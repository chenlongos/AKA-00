import {useEffect, useRef, useState} from "react";
import {api} from "../api";
import {useViewportScale} from "../hooks/useViewportScale";

interface CameraToggleProps {
    onStatusChange?: (on: boolean) => void;
}

const spinKeyframes = `
@keyframes camera-toggle-spin {
    to { transform: rotate(360deg); }
}`;
let _styleInjected = false;

const CameraToggle = ({onStatusChange}: CameraToggleProps) => {
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
            setOn(data.camera_on);
            onStatusChange?.(data.camera_on);
        }).catch(() => {}).finally(() => {
            if (mountedRef.current) setLoading(false);
        });
    };

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
