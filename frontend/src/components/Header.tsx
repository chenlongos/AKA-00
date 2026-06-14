import React from "react";
import {useNavigate} from "react-router-dom";
import {useViewportScale} from "../hooks/useViewportScale";

interface HeaderProps {
    title: string;
    subtitle?: React.ReactNode;
    actions?: React.ReactNode;
    /** 是否显示关闭按钮（返回上一页或首页） */
    showClose?: boolean;
    /** 关闭按钮跳转路径，默认 "/" */
    closeTo?: string;
}

/**
 * 粘性顶栏 — 用于子页面（如 /arm-angles, /wifi）
 */
const Header: React.FC<HeaderProps> = ({title, subtitle, actions, showClose = false, closeTo = "/"}) => {
    const {scalePx} = useViewportScale();
    const navigate = useNavigate();

    return (
        <div
            style={{
                background: "var(--color-bg-card)",
                padding: `${scalePx(12)} ${scalePx(16)}`,
                borderBottom: "1px solid var(--color-border-light)",
                display: "flex",
                justifyContent: "space-between",
                alignItems: "center",
                position: "sticky",
                top: 0,
                zIndex: 50,
            }}
        >
            <div style={{display: "flex", flexDirection: "column"}}>
                <h1 style={{fontSize: scalePx(16), fontWeight: 700, margin: 0}}>{title}</h1>
                {subtitle && (
                    <div style={{fontSize: scalePx(11), color: "var(--color-text-dim)", marginTop: 2}}>
                        {subtitle}
                    </div>
                )}
            </div>
            <div style={{display: "flex", gap: scalePx(10), alignItems: "center"}}>
                {actions}
                {showClose && (
                    <button
                        onClick={() => navigate(closeTo)}
                        style={{
                            fontSize: scalePx(13),
                            color: "var(--color-text-dim)",
                            background: "none",
                            border: "none",
                            cursor: "pointer",
                            padding: scalePx(4),
                        }}
                    >
                        ✕
                    </button>
                )}
            </div>
        </div>
    );
};

export default Header;
