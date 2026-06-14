import React from "react";
import {useViewportScale} from "../hooks/useViewportScale";

interface CardProps {
    children: React.ReactNode;
    /** 内边距倍数，默认 1（~14px） */
    pad?: number;
    /** 外边距方向，默认 bottom */
    marginBottom?: number;
    style?: React.CSSProperties;
}

/**
 * 卡片容器 — 统一圆角、背景、阴影
 */
const Card: React.FC<CardProps> = ({children, pad = 1, marginBottom = 12, style}) => {
    const {scalePx} = useViewportScale();
    const p = Math.round(14 * pad);
    return (
        <div
            style={{
                background: "var(--color-bg-card)",
                borderRadius: "var(--radius-md)",
                padding: scalePx(p),
                boxShadow: "0 1px 2px rgba(0,0,0,0.1)",
                marginBottom: marginBottom > 0 ? scalePx(marginBottom) : undefined,
                ...style,
            }}
        >
            {children}
        </div>
    );
};

export default Card;
