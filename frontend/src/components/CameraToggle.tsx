import {useEffect, useState} from "react";
import {api} from "../api";

interface CameraToggleProps {
    onStatusChange?: (on: boolean) => void;
}

const CameraToggle = ({onStatusChange}: CameraToggleProps) => {
    const [on, setOn] = useState(false);

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
                width: "44px",
                height: "22px",
                borderRadius: "11px",
                background: on ? "#22c55e" : "#334155",
                border: `1px solid ${on ? "#22c55e" : "#475569"}`,
                cursor: "pointer",
                position: "relative",
                transition: "background 0.2s",
            }}
        >
            <div style={{
                position: "absolute",
                top: "2px",
                left: on ? "23px" : "2px",
                width: "16px",
                height: "16px",
                borderRadius: "50%",
                background: "white",
                transition: "left 0.2s",
            }}/>
        </div>
    );
};

export default CameraToggle;