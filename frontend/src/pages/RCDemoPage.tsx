import {useCallback, useEffect, useRef, useState} from "react";
import {api, controlSocket} from "../api";
import CameraToggle from "../components/CameraToggle";
import Page from "../components/Page";
import {S} from "../styles";
import {useViewportScale} from "../hooks/useViewportScale";

const FPS_OPTIONS = [30, 20, 10];
const FPS_LABELS = ["高清", "均衡", "省流"];

const RCDemoPage = () => {
    const {scalePx, scale} = useViewportScale();
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [throttleDisplay, setThrottleDisplay] = useState(0);
    const [directionDisplay, setDirectionDisplay] = useState(0);
    const [cameraOn, setCameraOn] = useState(false);
    const [fpsIndex, setFpsIndex] = useState(0);
    const [wsConnected, setWsConnected] = useState(false);
    const [isLandscape, setIsLandscape] = useState(() => window.innerWidth > window.innerHeight);

    useEffect(() => {
        const check = () => setIsLandscape(window.innerWidth > window.innerHeight);
        window.addEventListener("resize", check);
        window.addEventListener("orientationchange", check);
        return () => { window.removeEventListener("resize", check); window.removeEventListener("orientationchange", check); };
    }, []);

    const throttleRef = useRef(0);
    const directionRef = useRef(0);
    const motorIntervalRef = useRef<number | null>(null);
    const lastCommandRef = useRef<{x: number, y: number} | null>(null);
    const throttleRefEl = useRef<HTMLDivElement>(null);
    const throttleActiveRef = useRef(false);
    const directionRefEl = useRef<HTMLDivElement>(null);
    const directionActiveRef = useRef(false);

    const sendCommand = useCallback(() => {
        const thr = Math.abs(throttleRef.current) < 3 ? 0 : throttleRef.current;
        const dir = Math.abs(directionRef.current) < 3 ? 0 : directionRef.current;
        if (lastCommandRef.current && Math.abs(lastCommandRef.current.x - dir) < 5 && Math.abs(lastCommandRef.current.y - thr) < 5) return;
        lastCommandRef.current = {x: dir, y: thr};
        controlSocket.sendJoystick(Math.round(dir * 0.5), thr);
    }, []);

    const startControl = useCallback(() => {
        if (motorIntervalRef.current !== null) return;
        sendCommand();
        motorIntervalRef.current = window.setInterval(sendCommand, 30);
    }, [sendCommand]);

    const stopControl = useCallback(() => {
        if (motorIntervalRef.current !== null) { clearInterval(motorIntervalRef.current); motorIntervalRef.current = null; }
        controlSocket.sendJoystick(0, 0);
        setLeftSpeed(0); setRightSpeed(0);
    }, []);

    useEffect(() => {
        controlSocket.connect(setWsConnected, (s) => { setLeftSpeed(s.left); setRightSpeed(s.right); });
        return () => { stopControl(); controlSocket.close(); navigator.sendBeacon("/api/camera/close"); };
    }, []);

    const stickSize = Math.round(38 * scale);
    const sliderLength = isLandscape ? Math.min(window.innerHeight * 0.55, Math.round(200 * scale)) : Math.min(window.innerWidth * 0.35, Math.round(150 * scale));
    const sliderWidth = Math.round(44 * scale);

    // 油门（垂直推）
    const handleThrottleMove = useCallback((_x: number, clientY: number) => {
        if (!throttleRefEl.current) return;
        const rect = throttleRefEl.current.getBoundingClientRect();
        const maxDist = rect.height / 2 - stickSize / 2 - 4;
        const dist = Math.max(-maxDist, Math.min(maxDist, clientY - (rect.top + rect.height / 2)));
        const thr = Math.round((-dist / maxDist) * 100);
        throttleRef.current = thr; setThrottleDisplay(thr);
        if (thr !== 0 && !throttleActiveRef.current) { throttleActiveRef.current = true; startControl(); }
    }, [startControl, stickSize]);

    const handleThrottleEnd = useCallback(() => { throttleRef.current = 0; throttleActiveRef.current = false; setThrottleDisplay(0); }, []);

    // 方向（水平推）
    const handleDirectionMove = useCallback((clientX: number, _y: number) => {
        if (!directionRefEl.current) return;
        const rect = directionRefEl.current.getBoundingClientRect();
        const maxDist = rect.width / 2 - stickSize / 2 - 4;
        const dist = Math.max(-maxDist, Math.min(maxDist, clientX - (rect.left + rect.width / 2)));
        const dir = Math.round((dist / maxDist) * 100);
        directionRef.current = dir; setDirectionDisplay(dir);
        if (dir !== 0 && !directionActiveRef.current) { directionActiveRef.current = true; startControl(); }
    }, [startControl, stickSize]);

    const handleDirectionEnd = useCallback(() => { directionRef.current = 0; directionActiveRef.current = false; setDirectionDisplay(0); }, []);

    const handleBrake = () => {
        throttleRef.current = 0; directionRef.current = 0;
        setThrottleDisplay(0); setDirectionDisplay(0);
        throttleActiveRef.current = false; directionActiveRef.current = false;
        stopControl();
    };

    const handleArm = async (action: "grab" | "release") => { try { await api.motor.action(action); } catch {} };

    const videoMaxW = isLandscape
        ? Math.min(window.innerWidth * 0.45, Math.round(400 * scale))
        : Math.min(window.innerWidth - 24, Math.round(360 * scale));

    const renderSlider = (
        refEl: React.RefObject<HTMLDivElement | null>,
        vertical: boolean, display: number,
        activeColor: string, idleColor: string,
        onMove: (cx: number, cy: number) => void, onEnd: () => void,
        label: string, posLabel: string, negLabel: string,
    ) => (
        <div style={{...S.col, gap: scalePx(4)}}>
            <span style={{fontSize: scalePx(9), color: "var(--color-text-dim)", textTransform: "uppercase", letterSpacing: "1px"}}>{label}</span>
            <div ref={refEl}
                 onPointerDown={e => { e.currentTarget.setPointerCapture(e.pointerId); onMove(e.clientX, e.clientY); }}
                 onPointerMove={e => onMove(e.clientX, e.clientY)}
                 onPointerUp={onEnd} onPointerLeave={onEnd} onPointerCancel={onEnd}
                 style={{
                     width: vertical ? sliderWidth : sliderLength, height: vertical ? sliderLength : sliderWidth,
                     borderRadius: "var(--radius-xl)", background: "var(--color-bg-card)", border: "2px solid var(--color-border)",
                     position: "relative", touchAction: "none", cursor: "pointer",
                 }}>
                <div style={{position: "absolute", left: vertical ? "50%" : 0, top: vertical ? 0 : "50%", transform: vertical ? "translateX(-50%)" : "translateY(-50%)", width: vertical ? "2px" : "100%", height: vertical ? "100%" : "1px", background: "var(--color-border-light)"}} />
                <div style={{
                    position: "absolute", width: stickSize, height: stickSize, borderRadius: "50%",
                    background: display !== 0 ? activeColor : idleColor, border: "3px solid var(--color-border)",
                    left: vertical ? "50%" : `${50 + display / 100 * 50}%`,
                    top: vertical ? `${50 - display / 100 * 50}%` : "50%",
                    transform: "translate(-50%, -50%)", boxShadow: "0 2px 8px rgba(0,0,0,0.35)", transition: display === 0 ? "background 0.3s" : "none",
                }} />
            </div>
            <span style={{fontSize: scalePx(10), color: "var(--color-text-muted)", fontWeight: 600}}>
                {display > 0 ? posLabel : display < 0 ? negLabel : "·"} {Math.abs(display)}%
            </span>
        </div>
    );

    return (
        <Page center padTop={6}>
            {/* 横屏 */}
            {isLandscape ? (
                <div style={{display: "flex", alignItems: "center", justifyContent: "center", gap: scalePx(6), minHeight: `calc(100dvh - var(--tab-bar-height) - 56px)`}}>
                    {renderSlider(directionRefEl, false, directionDisplay, "var(--color-primary)", "var(--color-bg-elevated)", handleDirectionMove, handleDirectionEnd, "方向", "→", "←")}
                    <div style={{...S.col, gap: scalePx(4), flex: 1}}>
                        <div style={{...S.row, gap: scalePx(8)}}>
                            <span style={S.muted}>摄像头</span><CameraToggle onStatusChange={setCameraOn} />
                            {cameraOn && <button onClick={() => setFpsIndex((fpsIndex + 1) % 3)} style={{background: fpsIndex > 0 ? "var(--color-success)" : "var(--color-bg-elevated)", border: `1px solid ${fpsIndex > 0 ? "var(--color-success)" : "var(--color-border)"}`, color: fpsIndex > 0 ? "#fff" : "var(--color-text-muted)", padding: `${scalePx(1)} ${scalePx(5)}`, borderRadius: "var(--radius-sm)", fontSize: scalePx(9), fontWeight: 600, cursor: "pointer"}}>{FPS_LABELS[fpsIndex]}</button>}
                        </div>
                        <div style={{borderRadius: "var(--radius-md)", overflow: "hidden", border: "2px solid var(--color-border)"}}>
                            {cameraOn ? <img src={`/api/camera/stream?fps=${FPS_OPTIONS[fpsIndex]}`} alt="Camera" style={{width: videoMaxW, height: Math.round(videoMaxW * 0.5625), objectFit: "cover", background: "#000", display: "block"}} />
                                : <div style={{width: videoMaxW, height: Math.round(videoMaxW * 0.5625), background: "var(--color-bg-card)", ...S.col, justifyContent: "center", color: "var(--color-text-muted)", fontSize: scalePx(12)}}>摄像头关闭</div>}
                        </div>
                        <div style={{...S.row, gap: scalePx(12), fontSize: scalePx(11)}}>
                            <span>左 <b style={S.success}>{leftSpeed >= 0 ? "+" : ""}{leftSpeed.toFixed(2)}</b></span>
                            <span>右 <b style={S.success}>{rightSpeed >= 0 ? "+" : ""}{rightSpeed.toFixed(2)}</b></span>
                            <span style={S.dot(wsConnected)} />
                        </div>
                    </div>
                    {renderSlider(throttleRefEl, true, throttleDisplay, "var(--color-success)", "var(--color-bg-elevated)", handleThrottleMove, handleThrottleEnd, "油门", "▲", "▼")}
                </div>
            ) : (
                /* 竖屏 */
                <div style={{...S.col, gap: scalePx(6)}}>
                    <div style={{display: "flex", justifyContent: "space-between", alignItems: "center", width: "100%", maxWidth: videoMaxW}}>
                        <h3 style={{fontSize: scalePx(14), fontWeight: 600, margin: 0}}>摇杆驾驶</h3>
                        <div style={{...S.row, gap: scalePx(6)}}>
                            {cameraOn && <button onClick={() => setFpsIndex((fpsIndex + 1) % 3)} style={{background: fpsIndex > 0 ? "var(--color-success)" : "var(--color-bg-elevated)", border: `1px solid ${fpsIndex > 0 ? "var(--color-success)" : "var(--color-border)"}`, color: fpsIndex > 0 ? "#fff" : "var(--color-text-muted)", padding: `${scalePx(1)} ${scalePx(5)}`, borderRadius: "var(--radius-sm)", fontSize: scalePx(9), fontWeight: 600, cursor: "pointer"}}>{FPS_LABELS[fpsIndex]}</button>}
                            <span style={S.muted}>摄像头</span><CameraToggle onStatusChange={setCameraOn} />
                        </div>
                    </div>
                    <div style={{borderRadius: "var(--radius-md)", overflow: "hidden", border: "2px solid var(--color-border)"}}>
                        {cameraOn ? <img src={`/api/camera/stream?fps=${FPS_OPTIONS[fpsIndex]}`} alt="Camera" style={{width: videoMaxW, height: Math.round(videoMaxW * 0.5625), objectFit: "cover", background: "#000", display: "block"}} />
                            : <div style={{width: videoMaxW, height: Math.round(videoMaxW * 0.5625), background: "var(--color-bg-card)", ...S.col, justifyContent: "center", color: "var(--color-text-muted)", fontSize: scalePx(12)}}>摄像头关闭</div>}
                    </div>
                    <div style={{...S.row, gap: scalePx(12), fontSize: scalePx(11)}}>
                        <span>左 <b style={S.success}>{leftSpeed >= 0 ? "+" : ""}{leftSpeed.toFixed(2)}</b></span>
                        <span>右 <b style={S.success}>{rightSpeed >= 0 ? "+" : ""}{rightSpeed.toFixed(2)}</b></span>
                        <span style={S.dot(wsConnected)} />
                    </div>
                    <div style={{display: "flex", gap: scalePx(16)}}>
                        {renderSlider(throttleRefEl, true, throttleDisplay, "var(--color-success)", "var(--color-bg-elevated)", handleThrottleMove, handleThrottleEnd, "油门", "▲", "▼")}
                        {renderSlider(directionRefEl, false, directionDisplay, "var(--color-primary)", "var(--color-bg-elevated)", handleDirectionMove, handleDirectionEnd, "方向", "→", "←")}
                    </div>
                </div>
            )}

            {/* 按钮组 */}
            <div style={{...S.col, gap: scalePx(6), maxWidth: isLandscape ? Math.round(videoMaxW + sliderLength + 120) : videoMaxW, marginTop: scalePx(8)}}>
                <div style={{display: "flex", gap: scalePx(8), width: "100%"}}>
                    <button onClick={() => handleArm("grab")} style={{flex: 1, padding: `${scalePx(8)} 0`, background: "linear-gradient(135deg, #16a34a, var(--color-success))", border: "none", color: "#fff", borderRadius: "var(--radius-md)", fontSize: scalePx(14), fontWeight: 700, letterSpacing: "1px", boxShadow: "0 3px 8px rgba(34,197,94,0.3)", cursor: "pointer", outline: "none"}}>✋ 抓取</button>
                    <button onClick={() => handleArm("release")} style={{flex: 1, padding: `${scalePx(8)} 0`, background: "linear-gradient(135deg, #2563eb, var(--color-primary))", border: "none", color: "#fff", borderRadius: "var(--radius-md)", fontSize: scalePx(14), fontWeight: 700, letterSpacing: "1px", boxShadow: "0 3px 8px rgba(59,130,246,0.3)", cursor: "pointer", outline: "none"}}>🤚 释放</button>
                </div>
                <button onClick={handleBrake} style={{width: "100%", padding: `${scalePx(7)} 0`, background: "var(--color-danger-soft)", border: "2px solid var(--color-danger)", color: "var(--color-danger)", borderRadius: "var(--radius-md)", fontSize: scalePx(13), fontWeight: 700, letterSpacing: "1px", cursor: "pointer", outline: "none"}}>⏹ 刹车</button>
            </div>
        </Page>
    );
};

export default RCDemoPage;
