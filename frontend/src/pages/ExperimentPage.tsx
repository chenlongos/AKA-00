import type {CSSProperties} from "react";
import {useViewportScale} from "../hooks/useViewportScale";

interface ExperimentItem {
    key: string;
    title: string;
    desc: string;
    tag: string;
    icon: string;
    /** 主题色（图标底、强调文字、hover 边框） */
    accent: string;
    accentSoft: string;
    /** 该实验只能用 HTTP 打开（HTTPS 下页面异常） */
    httpOnly?: boolean;
    sensor?: boolean;
}

const EXPERIMENTS: ExperimentItem[] = [
    {
        key: "gravity",
        title: "重力感应",
        desc: "倾斜手机来控制小车前进、后退与转向。",
        tag: "传感器",
        icon: "📱",
        accent: "#fbbf24",
        accentSoft: "rgba(251, 191, 36, 0.16)",
        sensor: true,
    },
    {
        key: "gesture",
        title: "手势识别",
        desc: "识别手掌与手指动作，用手势控制小车。",
        tag: "摄像头",
        icon: "✋",
        accent: "#34d399",
        accentSoft: "rgba(52, 211, 153, 0.16)",
    },
    {
        key: "pose",
        title: "姿势识别",
        desc: "跟踪人体关键点，识别人体姿态与动作。",
        tag: "摄像头",
        icon: "🧍",
        accent: "#f472b6",
        accentSoft: "rgba(244, 114, 182, 0.16)",
    },
    {
        key: "object-detection",
        title: "多目标识别",
        desc: "实时识别画面中的常见物体，并用框标出位置。",
        tag: "摄像头",
        icon: "🎯",
        accent: "#38bdf8",
        accentSoft: "rgba(56, 189, 248, 0.16)",
    },
    {
        key: "blockly",
        title: "图形化编程",
        desc: "拖动积木拼出指令，让小车执行动作、识别与巡线。",
        tag: "编程",
        icon: "🧩",
        accent: "#a78bfa",
        accentSoft: "rgba(167, 139, 250, 0.16)",
        httpOnly: true,
    },
];

/**
 * 实验入口页版本号：入口页放在 /assets/experiments/ 下走 1 天缓存，
 * 重新打包某个实验后改这里的值，浏览器就会立刻取新页面（旧缓存自然过期）。
 */
const EXP_VERSION = "20261113a";

/** 实验页地址：每个实验都是独立页面，用新标签页直接打开（不再用 iframe 包一层） */
function experimentUrl(exp: ExperimentItem): string {
    const protocol = exp.httpOnly ? "http:" : window.location.protocol;
    return `${protocol}//${window.location.host}/assets/experiments/${exp.key}/index.html?v=${EXP_VERSION}`;
}

/**
 * AI 感知实验导航页（/experiment）
 * 实验本体是独立的离线页面，点击卡片在新标签页打开。
 */
const ExperimentPage = () => {
    const {scalePx} = useViewportScale();
    // 摄像头只在安全上下文（HTTPS / localhost）可用；HTTP 访问时提示并提供切换入口
    const isHttps = window.location.protocol === "https:";
    const httpsUrl = `https://${window.location.host}${window.location.pathname}`;

    return (
        <div style={{height: "100%", display: "flex", flexDirection: "column", background: "var(--color-bg)"}}>
            <div style={{
                display: "flex",
                flexDirection: "column",
                gap: scalePx(5),
                padding: `${scalePx(14)} ${scalePx(16)}`,
                background: "var(--color-bg-card)",
                borderBottom: "1px solid var(--color-border-light)",
            }}>
                <div style={{display: "flex", alignItems: "center", gap: scalePx(8), flexWrap: "wrap"}}>
                    <h1 style={{fontSize: scalePx(18), fontWeight: 700, margin: 0}}>AI 感知实验</h1>
                    <span style={{
                        padding: `${scalePx(2)} ${scalePx(8)}`,
                        borderRadius: "var(--radius-full)",
                        background: "rgba(34, 197, 94, 0.14)",
                        color: "#4ade80",
                        fontSize: scalePx(10),
                        fontWeight: 700,
                    }}>离线可用</span>
                </div>
                <span style={{fontSize: scalePx(11), lineHeight: 1.65, color: "var(--color-text-dim)"}}>
                    模型与页面都在小车上，无需联网 · 点击卡片在新标签页打开 · 摄像头实验需允许权限（自签证书请先手动信任）
                </span>
            </div>

            <div style={{flex: 1, overflowY: "auto", padding: `${scalePx(14)} ${scalePx(14)} ${scalePx(24)}`}}>
                {!isHttps && (
                    <div style={{
                        display: "flex",
                        alignItems: "center",
                        justifyContent: "space-between",
                        flexWrap: "wrap",
                        gap: scalePx(10),
                        marginBottom: scalePx(14),
                        padding: `${scalePx(10)} ${scalePx(12)}`,
                        background: "rgba(245, 158, 11, 0.12)",
                        border: "1px solid rgba(245, 158, 11, 0.5)",
                        borderRadius: "var(--radius-lg)",
                    }}>
                        <div style={{display: "flex", flexDirection: "column", gap: scalePx(2), minWidth: 0, flex: 1}}>
                            <span style={{fontSize: scalePx(13), fontWeight: 600, color: "#fbbf24"}}>
                                当前是 HTTP 访问
                            </span>
                            <span style={{fontSize: scalePx(11), lineHeight: 1.6, color: "var(--color-text-muted)"}}>
                                摄像头类实验需要 HTTPS 才能授权使用，请点击右侧按钮切换。
                            </span>
                        </div>
                        <a href={httpsUrl} style={{
                            flexShrink: 0,
                            padding: `${scalePx(7)} ${scalePx(14)}`,
                            borderRadius: "var(--radius-full)",
                            background: "#f59e0b",
                            color: "#1c1917",
                            fontSize: scalePx(13),
                            fontWeight: 700,
                            textDecoration: "none",
                            whiteSpace: "nowrap",
                        }}>
                            切换到 HTTPS →
                        </a>
                    </div>
                )}

                <div className="exp-grid">
                    {EXPERIMENTS.map((exp, i) => (
                        <a
                            key={exp.key}
                            href={experimentUrl(exp)}
                            target="_blank"
                            rel="noreferrer"
                            className="exp-card"
                            style={{
                                "--exp-accent": exp.accent,
                                background: `radial-gradient(120% 100% at 100% 0%, ${exp.accentSoft} 0%, transparent 58%), var(--color-bg-card)`,
                                borderRadius: "var(--radius-xl)",
                                padding: scalePx(14),
                                color: "var(--color-text)",
                                animation: `fadeIn 0.3s ease-out ${i * 45}ms both`,
                            } as CSSProperties}
                        >
                            <div style={{display: "flex", alignItems: "center", justifyContent: "space-between", gap: scalePx(8)}}>
                                <span style={{
                                    display: "grid",
                                    placeItems: "center",
                                    width: scalePx(42),
                                    height: scalePx(42),
                                    borderRadius: scalePx(13),
                                    background: exp.accentSoft,
                                    border: `1px solid ${exp.accent}`,
                                    boxShadow: `0 0 16px ${exp.accentSoft}`,
                                    fontSize: scalePx(21),
                                    lineHeight: 1,
                                }}>{exp.icon}</span>
                                <span style={{
                                    flexShrink: 0,
                                    padding: `${scalePx(2)} ${scalePx(8)}`,
                                    borderRadius: "var(--radius-full)",
                                    background: exp.sensor ? "rgba(245, 158, 11, 0.14)" : "var(--color-primary-soft)",
                                    color: exp.sensor ? "#fbbf24" : "#93c5fd",
                                    fontSize: scalePx(10),
                                    fontWeight: 600,
                                }}>{exp.tag}</span>
                            </div>
                            <span style={{marginTop: scalePx(10), fontSize: scalePx(15), fontWeight: 700}}>{exp.title}</span>
                            <span style={{
                                marginTop: scalePx(4),
                                flex: 1,
                                minHeight: scalePx(36),
                                fontSize: scalePx(12),
                                lineHeight: 1.6,
                                color: "var(--color-text-muted)",
                            }}>{exp.desc}</span>
                            <span style={{
                                marginTop: scalePx(10),
                                display: "flex",
                                alignItems: "center",
                                gap: scalePx(4),
                                fontSize: scalePx(12),
                                fontWeight: 700,
                                color: exp.accent,
                            }}>
                                开始实验 <span className="exp-arrow">→</span>
                            </span>
                        </a>
                    ))}
                </div>
            </div>
        </div>
    );
};

export default ExperimentPage;
