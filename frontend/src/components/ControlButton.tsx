import {useState} from "react";
import {useViewportScale} from "../hooks/useViewportScale";

type ButtonProps = {
    children: React.ReactNode;
    onPressStart?: () => void;
    onPressEnd?: () => void;
    onClick?: () => void;
    variant?: "primary" | "danger" | "success" | "secondary";
    size?: "square" | "wide";
    disabled?: boolean;
    loading?: boolean;
};

const colorMap = {
    primary: "#2563eb",
    danger: "#ef4444",
    success: "#22c55e",
    secondary: "#1e293b",
};

const pressedColorMap = {
    primary: "#1d4ed8",
    danger: "#dc2626",
    success: "#16a34a",
    secondary: "#0f172a",
};

const ControlButton = ({
                           children,
                           variant = "primary",
                           size = "square",
                           onPressStart,
                           onPressEnd,
                           onClick,
                           disabled = false,
                           loading = false,
                       }: ButtonProps) => {
    const [isPressed, setIsPressed] = useState(false);
    const {scalePx} = useViewportScale();
    const baseSize =
        size === "square"
            ? {width: "22vw", height: "22vw", maxWidth: scalePx(110), maxHeight: scalePx(110)}
            : {width: "40vw", height: scalePx(60), maxWidth: scalePx(200)};

    const handlePressStart = () => {
        setIsPressed(true);
        onPressStart?.();
    };

    const handlePressEnd = () => {
        setIsPressed(false);
        onPressEnd?.();
    };

    return (
        <button
            onPointerDown={handlePressStart}
            onPointerUp={handlePressEnd}
            onPointerLeave={handlePressEnd}
            onPointerCancel={handlePressEnd}
            onContextMenu={(e) => e.preventDefault()}
            onClick={(e) => {
                e.preventDefault(); // 防止默认行为（如表单提交）
                onClick?.();
            }}
            style={{
                ...baseSize,
                borderRadius: scalePx(18),
                border: "none",
                background: isPressed ? pressedColorMap[variant] : colorMap[variant],
                color: "white",
                fontWeight: "bold",
                fontSize: scalePx(16),
                transition: "all 0.15s ease",
                boxShadow: isPressed
                    ? "0 3px 8px rgba(0,0,0,0.4)"
                    : "0 6px 15px rgba(0,0,0,0.5)",
                transform: isPressed ? "scale(0.96)" : "scale(1)",
                touchAction: "none",
                opacity: disabled || loading ? 0.5 : 1,
                pointerEvents: disabled || loading ? "none" : "auto",
            }}
        >
            {loading ? (
                <span style={{
                    display: "inline-block",
                    animation: "spin 1s linear infinite",
                }}>
                    ↻
                </span>
            ) : children}
        </button>
    )
}

export default ControlButton