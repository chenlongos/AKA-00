import {useEffect, useState} from "react";
import {api} from "../api";
import {useViewportScale} from "../hooks/useViewportScale";

interface CameraToggleProps {
    onStatusChange?: (on: boolean) => void;
}

const CameraToggle = ({onStatusChange}: CameraToggleProps) => {
    const [on, setOn] = useState(false);
    const {scalePx} = useViewportScale();

    useEffect(() => {
        api.camera.status().then(data => {
            setOn(data.camera_on);
            onStatusChange?.(data.camera_on);
        }).catch(() => {});
    }, []);

    const toggle = () => {
        const req = on ? api.camera.close() : api.camera.open();
        req.then(data => {
            setOn(data.camera_on);
            onStatusChange?.(data.camera_on);
        }).catch(() => {});
    };

    return (
        <div
            onClick={toggle}
            style={{
                width: scalePx(44),
                height: scalePx(24),
                borderRadius: scalePx(12),
                background: on ? "var(--color-success)" : "var(--color-bg-elevated)",
                cursor: "pointer",
                position: "relative",
                transition: "background 0.2s",
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
                background: "white",
                transition: "left 0.2s ease",
                boxShadow: "0 1px 3px rgba(0,0,0,0.3)",
            }}/>
        </div>
    );
};

export default CameraToggle;
