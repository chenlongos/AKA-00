import {useEffect, useRef, useState} from "react";
import {api, controlSocket} from "../api";
import ControlButton from "../components/ControlButton.tsx";
import CameraToggle from "../components/CameraToggle";
import Page from "../components/Page";
import {S} from "../styles";
import {useViewportScale} from "../hooks/useViewportScale";

const BaseControlPage = () => {
    const {scalePx, scale} = useViewportScale();
    const [ip, setIp] = useState("获取中...");
    const [status, setStatus] = useState("准备就绪");
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [wsConnected, setWsConnected] = useState(false);
    const currentActionRef = useRef<string | null>(null);

    useEffect(() => {
        controlSocket.connect(setWsConnected, (s) => { setLeftSpeed(s.left); setRightSpeed(s.right); });
        return () => { controlSocket.close(); };
    }, []);

    useEffect(() => {
        const processHash = () => {
            const hash = window.location.hash;
            if (hash) {
                api.motor.rawCommand(hash).catch(console.error);
                window.history.replaceState(null, document.title, window.location.pathname);
            }
        };
        processHash();
        window.addEventListener('hashchange', processHash);
        return () => window.removeEventListener('hashchange', processHash);
    }, []);

    useEffect(() => {
        api.system.ip().then(data => { setIp("IP: " + data.ip); setStatus("准备就绪"); }).catch(() => setStatus("获取 IP 失败"));
        api.base.pwmChannels().then(data => api.base.savePwmChannels(data.pwm_channels).catch(() => {}));
        api.base.reinitialize().catch(() => {});
    }, []);

    const send = async (action: string, speed: number = 50) => {
        setStatus("执行: " + action);
        if (action === "stop") { controlSocket.sendJoystick(0, 0); }
        else if (action === "up") { controlSocket.sendJoystick(0, speed); }
        else if (action === "down") { controlSocket.sendJoystick(0, -speed); }
        else if (action === "left") { controlSocket.sendJoystick(-speed, 0); }
        else if (action === "right") { controlSocket.sendJoystick(speed, 0); }
        else { try { await api.motor.action(action, speed); } catch (err) { setStatus("错误: " + err); } }
    };

    const handlePressStart = (action: string, speed?: number) => { currentActionRef.current = action; send(action, speed); };
    const handlePressEnd = () => { currentActionRef.current = null; send("stop"); };

    const redirect = async () => {
        try {
            const data = await api.system.ip();
            if (!data?.ip) throw new Error("无IP数据");
            window.location.replace(`https://labs.chenlongrobot.com/?ip=${encodeURIComponent(data.ip)}`);
        } catch { alert("无法获取IP，请稍后重试"); }
    };

    // 十字方向键尺寸
    const dpadSize = Math.min(window.innerWidth * 0.72, Math.round(280 * scale));
    const dpadGap = Math.round(6 * scale);
    const cellSize = Math.round((dpadSize - dpadGap * 2) / 3);
    const btnFontSize = Math.round(16 * scale);

    const dpadBtn = (dir: string, label: string, bgColor: string) => (
        <button
            onPointerDown={(e) => { e.preventDefault(); e.stopPropagation(); handlePressStart(dir, dir === "left" || dir === "right" ? 25 : 50); }}
            onPointerUp={(e) => { e.preventDefault(); handlePressEnd(); }}
            onPointerLeave={handlePressEnd}
            onPointerCancel={handlePressEnd}
            style={{
                width: cellSize, height: cellSize,
                borderRadius: scalePx(18),  // 匹配原始 ControlButton 圆角
                border: "none",
                background: bgColor, color: "#fff",
                fontWeight: "bold", fontSize: btnFontSize,
                touchAction: "none",
                display: "flex", alignItems: "center", justifyContent: "center",
                boxShadow: "0 6px 15px rgba(0,0,0,0.5)",  // 匹配原始 ControlButton 阴影
                cursor: "pointer", outline: "none",
                transition: "all 0.15s ease",
            }}
        >
            {label}
        </button>
    );

    return (
        <Page center padTop={10}>
            {/* Header */}
            <div style={{textAlign: "center", width: "100%"}}>
                <h2 style={{fontSize: scalePx(17), fontWeight: 700, margin: 0}}>AKA-00 控制台</h2>
                <div style={{
                    fontSize: scalePx(15), fontWeight: 500,
                    color: "var(--color-text-muted)", marginTop: scalePx(4),
                    fontFamily: "monospace",
                    userSelect: "text", WebkitUserSelect: "text",
                    WebkitTouchCallout: "default",
                }}>{ip}</div>
            </div>

            {/* 状态行 */}
            <div style={{...S.rowBetween, width: "100%", maxWidth: dpadSize, marginTop: scalePx(4)}}>
                <div style={{...S.row, gap: scalePx(6)}}>
                    <span style={S.dot(wsConnected)} />
                    <span style={S.muted}>{status}</span>
                </div>
                <div style={{...S.row, gap: scalePx(6)}}>
                    <span style={S.muted}>摄像头</span>
                    <CameraToggle />
                </div>
            </div>

            {/* 十字方向键 3x3 */}
            <div style={{display: "grid", gridTemplateColumns: `repeat(3, ${cellSize}px)`, gridTemplateRows: `repeat(3, ${cellSize}px)`, gap: dpadGap, width: dpadSize, height: dpadSize, marginTop: scalePx(4)}}>
                <div />{dpadBtn("up", "▲", "#2563eb")}<div />
                {dpadBtn("left", "◀", "#2563eb")}
                <button onPointerDown={(e) => { e.preventDefault(); handlePressStart("stop"); }}
                        style={{width: cellSize, height: cellSize, borderRadius: scalePx(18), border: "none", background: "#ef4444", color: "#fff", fontWeight: "bold", fontSize: Math.round(12 * scale), touchAction: "none", display: "flex", alignItems: "center", justifyContent: "center", boxShadow: "0 6px 15px rgba(0,0,0,0.5)", cursor: "pointer", outline: "none", letterSpacing: "1px", transition: "all 0.15s ease"}}>
                    停止
                </button>
                {dpadBtn("right", "▶", "#2563eb")}
                <div />{dpadBtn("down", "▼", "#2563eb")}<div />
            </div>

            {/* 速度 */}
            <div style={{...S.row, gap: scalePx(20), padding: `${scalePx(6)} ${scalePx(16)}`, background: "var(--color-bg-card)", borderRadius: "var(--radius-full)", fontSize: scalePx(12), marginTop: scalePx(4)}}>
                <span>左 <b style={S.success}>{leftSpeed.toFixed(1)}</b> m/s</span>
                <span>右 <b style={S.success}>{rightSpeed.toFixed(1)}</b> m/s</span>
            </div>

            {/* 抓取/释放 */}
            <div style={{display: "flex", gap: scalePx(10), width: "100%", maxWidth: dpadSize, marginTop: scalePx(4)}}>
                <ControlButton variant="success" size="full" onClick={() => send("grab")}>🤏 抓取</ControlButton>
                <ControlButton variant="secondary" size="full" onClick={() => send("release")}>👐 释放</ControlButton>
            </div>

            {/* 辅助入口 */}
            <div style={{display: "flex", gap: scalePx(10), flexWrap: "wrap", justifyContent: "center", maxWidth: dpadSize, marginTop: scalePx(16)}}>
                <ControlButton size="wide" variant="primary" onClick={() => window.location.href = "/gravity"}>📱 重力遥控</ControlButton>
                <ControlButton size="wide" variant="secondary" onClick={() => redirect()}>🧪 试验平台</ControlButton>
            </div>
        </Page>
    );
};

export default BaseControlPage;
