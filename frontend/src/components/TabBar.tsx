import {useLocation, useNavigate} from "react-router-dom";
import {useViewportScale} from "../hooks/useViewportScale";

interface Tab {
    path: string;
    label: string;
    icon: string;
    activeIcon: string;
}

const TABS: Tab[] = [
    {path: "/", label: "控制台", icon: "🏠", activeIcon: "🏠"},
    {path: "/rc", label: "摇杆驾驶", icon: "🕹️", activeIcon: "🕹️"},
    {path: "/demo", label: "Demo", icon: "📦", activeIcon: "📦"},
    {path: "/settings", label: "设置", icon: "⚙️", activeIcon: "⚙️"},
];

const TabBar = () => {
    const location = useLocation();
    const navigate = useNavigate();
    const {scalePx} = useViewportScale();

    return (
        <nav
            style={{
                display: "flex",
                justifyContent: "space-around",
                alignItems: "center",
                background: "var(--color-bg-card)",
                borderTop: "1px solid var(--color-border-light)",
                paddingBottom: "var(--safe-bottom)",
                height: `calc(var(--tab-bar-height) + var(--safe-bottom))`,
                minHeight: `calc(var(--tab-bar-height) + var(--safe-bottom))`,
                flexShrink: 0,
                zIndex: 1000,
            }}
        >
            {TABS.map((tab) => {
                const isActive = location.pathname === tab.path;
                return (
                    <button
                        key={tab.path}
                        onClick={() => navigate(tab.path)}
                        style={{
                            display: "flex",
                            flexDirection: "column",
                            alignItems: "center",
                            justifyContent: "center",
                            gap: "2px",
                            padding: `${scalePx(4)} ${scalePx(8)}`,
                            background: "transparent",
                            border: "none",
                            color: isActive ? "var(--color-primary)" : "var(--color-text-muted)",
                            fontSize: scalePx(10),
                            fontWeight: isActive ? 600 : 400,
                            cursor: "pointer",
                            transition: "color 0.2s ease",
                            minWidth: scalePx(52),
                            WebkitTapHighlightColor: "transparent",
                            outline: "none",
                            position: "relative",
                        }}
                    >
                        <span style={{fontSize: scalePx(20), lineHeight: 1}}>
                            {tab.icon}
                        </span>
                        <span style={{lineHeight: 1}}>{tab.label}</span>
                        {isActive && (
                            <span
                                style={{
                                    position: "absolute",
                                    top: 0,
                                    left: "50%",
                                    transform: "translateX(-50%)",
                                    width: scalePx(20),
                                    height: "2px",
                                    borderRadius: "1px",
                                    background: "var(--color-primary)",
                                }}
                            />
                        )}
                    </button>
                );
            })}
        </nav>
    );
};

export default TabBar;
