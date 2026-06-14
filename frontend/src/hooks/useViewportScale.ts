import {useCallback, useEffect, useState} from "react";

// 基准 375px（移动端设计标准宽度），以此为基准等比缩放
// 更窄的屏幕缩小，更宽的屏幕适度放大（上限 1.2，避免平板等大屏上 UI 元素过大）
const DEFAULT_BASE = 375;
const DEFAULT_MAX = 1.5;
const DEFAULT_MIN = 0.7;

function computeScale(base: number, max: number, min: number): number {
    // 使用窗口宽度作为计算基准（保持竖屏设计的缩放一致性）
    const w = window.innerWidth;
    const h = window.innerHeight;
    // 优先使用宽度，但在极端横屏时使用高度以防止 UI 垂直溢出
    const ref = w < h ? w : Math.max(w * 0.55, h * 0.85);
    return Math.min(Math.max(min, ref / base), max);
}

export function useViewportScale(
    baseRef = DEFAULT_BASE,
    maxScale = DEFAULT_MAX,
    minScale = DEFAULT_MIN,
) {
    const [scale, setScale] = useState(() => computeScale(baseRef, maxScale, minScale));

    useEffect(() => {
        const onResize = () => setScale(computeScale(baseRef, maxScale, minScale));
        window.addEventListener("resize", onResize);
        window.addEventListener("orientationchange", onResize);
        return () => {
            window.removeEventListener("resize", onResize);
            window.removeEventListener("orientationchange", onResize);
        };
    }, [baseRef, maxScale, minScale]);

    /** 将 px 值乘以 scale 并返回 "Xpx" 字符串，用于 inline style */
    const scalePx = useCallback(
        (px: number): string => `${Math.round(px * scale)}px`,
        [scale],
    );

    /** 将数值乘以 scale 并返回整数，用于复合运算或 React 自动 px 属性 */
    const scaleValue = useCallback(
        (val: number): number => Math.round(val * scale),
        [scale],
    );

    return {scale, scalePx, scaleValue};
}
