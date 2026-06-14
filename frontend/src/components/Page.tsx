import React from "react";
import {useViewportScale} from "../hooks/useViewportScale";

interface PageProps {
    children: React.ReactNode;
    /** 是否居中内容（默认 true） */
    center?: boolean;
    /** 顶部 padding */
    padTop?: number;
}

/**
 * 页面容器 — 统一的外壳，处理 padding、动画、滚动
 */
const Page: React.FC<PageProps> = ({children, center = true, padTop = 12}) => {
    const {scalePx} = useViewportScale();
    return (
        <div
            style={{
                padding: `${scalePx(padTop)} ${scalePx(12)} ${scalePx(24)}`,
                color: "var(--color-text)",
                animation: "fadeIn 0.25s ease-out",
                ...(center ? {display: "flex", flexDirection: "column", alignItems: "center"} : {}),
            }}
        >
            {children}
        </div>
    );
};

export default Page;
