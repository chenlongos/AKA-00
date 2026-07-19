import {useEffect, useState} from "react";
import Card from "../components/Card";
import Header from "../components/Header";
import {S} from "../styles";
import {useViewportScale} from "../hooks/useViewportScale";
import {config} from "../api";

const SpeedConfigPage = () => {
    const {scalePx} = useViewportScale();
    const [forwardSpeed, setForwardSpeed] = useState(50);
    const [turnSpeed, setTurnSpeed] = useState(50);
    const [saved, setSaved] = useState(false);

    useEffect(() => {
        config.speed().then(c => {
            if (c.forward_speed) setForwardSpeed(Math.min(60, Math.max(20, c.forward_speed)));
            if (c.turn_speed) setTurnSpeed(Math.min(60, Math.max(20, c.turn_speed)));
        }).catch(() => {});
    }, []);

    const save = () => {
        config.saveSpeed(forwardSpeed, turnSpeed).then(() => {
            setSaved(true);
            setTimeout(() => setSaved(false), 2000);
        });
    };

    const sliderStyle: React.CSSProperties = {
        width: "100%", accentColor: "var(--color-primary)",
    };

    return (
        <div style={{
            minHeight: "100dvh", overflowY: "auto",
            "--color-bg": "#f2f2f7",
            "--color-bg-card": "#ffffff",
            "--color-bg-elevated": "#e5e5ea",
            "--color-text": "#1c1c1e",
            "--color-text-dim": "#8e8e93",
            "--color-text-muted": "#6e6e73",
            "--color-border": "#d1d1d6",
            "--color-border-light": "rgba(0,0,0,0.08)",
            "--color-primary": "#007aff",
            "--color-success": "#34c759",
            "--color-danger": "#ff3b30",
            color: "#1c1c1e",
            background: "#f2f2f7",
        } as React.CSSProperties}>
            <Header title="行驶速度" showClose closeTo="/settings" />
            <div style={{padding: scalePx(16), maxWidth: scalePx(420), margin: "0 auto"}}>
                <Card marginBottom={10}>
                    <div style={{padding: `${scalePx(8)} 0`}}>
                        <div style={{marginBottom: scalePx(14)}}>
                            <div style={{display: "flex", justifyContent: "space-between", marginBottom: 4}}>
                                <span style={{fontSize: scalePx(14), fontWeight: 600}}>前进速度</span>
                                <span style={{...S.success, fontSize: scalePx(16)}}>{forwardSpeed}</span>
                            </div>
                            <div style={S.muted}>控制遥控器上下方向的速度百分比</div>
                            <input type="range" min={20} max={60} step={5} value={forwardSpeed}
                                   onChange={e => setForwardSpeed(Number(e.target.value))}
                                   style={sliderStyle} />
                        </div>

                        <div style={{marginBottom: scalePx(14)}}>
                            <div style={{display: "flex", justifyContent: "space-between", marginBottom: 4}}>
                                <span style={{fontSize: scalePx(14), fontWeight: 600}}>转向速度</span>
                                <span style={{...S.success, fontSize: scalePx(16)}}>{turnSpeed}</span>
                            </div>
                            <div style={S.muted}>控制遥控器左右方向的速度百分比</div>
                            <input type="range" min={20} max={60} step={5} value={turnSpeed}
                                   onChange={e => setTurnSpeed(Number(e.target.value))}
                                   style={sliderStyle} />
                        </div>

                        <div style={{display: "flex", gap: scalePx(8)}}>
                            <button onClick={save} style={{
                                flex: 1, padding: `${scalePx(10)} 0`,
                                background: saved ? "var(--color-success)" : "var(--color-primary)",
                                color: "#fff", border: "none", borderRadius: "var(--radius-md)",
                                fontSize: scalePx(14), fontWeight: 600, cursor: "pointer",
                            }}>
                                {saved ? "✓ 已保存" : "保存设置"}
                            </button>
                            <button onClick={() => {
                                setForwardSpeed(50); setTurnSpeed(50);
                                config.saveSpeed(50, 50);
                                setSaved(true); setTimeout(() => setSaved(false), 2000);
                            }} style={{
                                padding: `${scalePx(10)} ${scalePx(16)}`,
                                background: "var(--color-bg-card)", color: "var(--color-text-dim)",
                                border: "1px solid var(--color-border)", borderRadius: "var(--radius-md)",
                                fontSize: scalePx(13), cursor: "pointer",
                            }}>
                                恢复默认
                            </button>
                        </div>
                    </div>
                </Card>
            </div>
        </div>
    );
};

export default SpeedConfigPage;
