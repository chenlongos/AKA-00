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
                height: scalePx(22),
                borderRadius: scalePx(11),
                background: on ? "#22c55e" : "#334155",
                border: `1px solid ${on ? "#22c55e" : "#475569"}`,
                cursor: "pointer",
                position: "relative",
                transition: "background 0.2s",
            }}
        >
            <div style={{
                position: "absolute",
                top: scalePx(2),
                left: on ? scalePx(23) : scalePx(2),
                width: scalePx(16),
                height: scalePx(16),
                borderRadius: "50%",
                background: "white",
                transition: "left 0.2s",
            }}/>
        </div>
    );
};

export default CameraToggle;