import React from "react";

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
const Page: React.FC<PageProps> = ({children, center = true}) => {
    return (
        <div
            style={{
                height: "100%",
                width: "100%",
                padding: "0 30px 0 30px",
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
