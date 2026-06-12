import {useCallback, useEffect, useState} from "react";

// 基准 375px（主流手机宽度），屏幕更小时等比缩小，更大时保持原始大小
const DEFAULT_BASE = 375;
const DEFAULT_MAX = 1.0;
const DEFAULT_MIN = 0.75;

function computeScale(base: number, max: number, min: number): number {
    return Math.min(
        Math.max(min, Math.min(window.innerWidth, window.innerHeight) / base),
        max,
    );
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
        return () => window.removeEventListener("resize", onResize);
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
