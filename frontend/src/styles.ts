import React from "react";

/**
 * 常用 inline style 片段，减少 JSX 中重复对象字面量。
 * 这些是纯对象，不依赖 hooks，可在任何地方直接展开使用。
 */

export const S = {
    /** flex 垂直居中布局 */
    col: {
        display: "flex",
        flexDirection: "column",
        alignItems: "center",
    } as React.CSSProperties,

    /** flex 水平居中 */
    row: {
        display: "flex",
        alignItems: "center",
    } as React.CSSProperties,

    /** flex 水平两端对齐 */
    rowBetween: {
        display: "flex",
        justifyContent: "space-between",
        alignItems: "center",
    } as React.CSSProperties,

    /** 文字 muted */
    muted: {
        fontSize: "var(--font-sm)",
        color: "var(--color-text-dim)",
    } as React.CSSProperties,

    /** 成功色文字 */
    success: {
        color: "var(--color-success)",
        fontWeight: "bold",
    } as React.CSSProperties,

    /** 迷你连接状态圆点 */
    dot: (on: boolean): React.CSSProperties => ({
        width: 6,
        height: 6,
        borderRadius: "50%",
        background: on ? "var(--color-success)" : "var(--color-danger)",
        display: "inline-block",
    }),

    /** 分割线 */
    divider: (width = "60%", maxWidth = 250): React.CSSProperties => ({
        width,
        maxWidth,
        height: 1,
        background: "var(--color-border-light)",
        margin: "20px auto",
    }),
};
