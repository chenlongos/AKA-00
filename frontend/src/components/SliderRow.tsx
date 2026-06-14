import React from "react";
import {useViewportScale} from "../hooks/useViewportScale";

interface SliderRowProps {
    label: string;
    value: number;
    min: number;
    max: number;
    onChange: (value: number) => void;
    /** 值的后缀，如 "°" */
    suffix?: string;
    /** 值的颜色 */
    valueColor?: string;
}

/**
 * 标签 + 滑块 + 数值 一行
 * 在 ArmAnglesPage 中大量复用
 */
const SliderRow: React.FC<SliderRowProps> = ({
    label, value, min, max, onChange, suffix = "", valueColor = "var(--color-primary)",
}) => {
    const {scalePx} = useViewportScale();

    return (
        <div style={{marginBottom: scalePx(12)}}>
            <div style={{display: "flex", justifyContent: "space-between", marginBottom: scalePx(4)}}>
                <span style={{fontSize: scalePx(13), color: "var(--color-text)"}}>{label}</span>
                <span style={{fontSize: scalePx(13), fontWeight: 600, color: valueColor}}>
                    {value}{suffix}
                </span>
            </div>
            <input
                type="range"
                min={min}
                max={max}
                value={value}
                onChange={(e) => onChange(parseInt(e.target.value))}
                style={{width: "100%", accentColor: "var(--color-primary)"}}
            />
        </div>
    );
};

export default SliderRow;
