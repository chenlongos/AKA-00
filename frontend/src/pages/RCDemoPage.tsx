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
    const [throttle, setThrottle] = useState(0);
    const [rotation, setRotation] = useState(0);
    const [cameraOn, setCameraOn] = useState(false);
    const [fpsIndex, setFpsIndex] = useState(0);
    const [wsConnected, setWsConnected] = useState(false);
    const [isLandscape, setIsLandscape] = useState(() => window.innerWidth > window.innerHeight);

    const throttleRef = useRef(0);
    const rotationRef = useRef(0);
    const intervalRef = useRef<number | null>(null);
    const lastCmdRef = useRef<{x: number, y: number} | null>(null);
    const thrEl = useRef<HTMLDivElement>(null);
    const thrActive = useRef(false);
    const rotEl = useRef<HTMLDivElement>(null);
    const rotActive = useRef(false);

    useEffect(() => {
        const check = () => setIsLandscape(window.innerWidth > window.innerHeight);
        window.addEventListener("resize", check);
        window.addEventListener("orientationchange", check);
        return () => { window.removeEventListener("resize", check); window.removeEventListener("orientationchange", check); };
    }, []);

    const sendCommand = useCallback(() => {
        const t = Math.abs(throttleRef.current) < 3 ? 0 : throttleRef.current;
        const r = Math.abs(rotationRef.current) < 3 ? 0 : rotationRef.current;
        if (lastCmdRef.current && Math.abs(lastCmdRef.current.x - r) < 5 && Math.abs(lastCmdRef.current.y - t) < 5) return;
        lastCmdRef.current = {x: r, y: t};
        controlSocket.sendJoystick(Math.round(r * 0.5), t);
    }, []);

    const startMotor = useCallback(() => {
        if (intervalRef.current !== null) return;
        sendCommand();
        intervalRef.current = window.setInterval(sendCommand, 30);
    }, [sendCommand]);

    const stopMotor = useCallback(() => {
        if (intervalRef.current !== null) { clearInterval(intervalRef.current); intervalRef.current = null; }
        controlSocket.sendJoystick(0, 0);
        setLeftSpeed(0); setRightSpeed(0);
    }, []);

    useEffect(() => {
        controlSocket.connect(setWsConnected, (s) => { setLeftSpeed(s.left); setRightSpeed(s.right); });
        return () => { stopMotor(); controlSocket.close(); navigator.sendBeacon("/api/camera/close"); };
    }, []);

    const handleBrake = () => {
        throttleRef.current = 0; rotationRef.current = 0;
        setThrottle(0); setRotation(0);
        thrActive.current = false; rotActive.current = false;
        stopMotor();
    };

    const handleArm = async (a: "grab" | "release") => { try { await api.motor.action(a); } catch {} };

    // ---- 控制条尺寸 ----
    const stickR = Math.round(18 * scale);

    // 竖屏
    const pThrottleW = Math.round(50 * scale);
    const pThrottleH = Math.min(Math.round(window.innerHeight * 0.28), Math.round(200 * scale));
    const pRotW = Math.round((window.innerWidth - 48) / 2);
    const pRotH = Math.round(52 * scale);

    // 横屏 — 控制条更大
    const lThrottleW = Math.round(60 * scale);
    const lThrottleH = Math.min(Math.round(window.innerHeight * 0.5), Math.round(260 * scale));
    const lRotW = Math.round(180 * scale);
    const lRotH = Math.round(60 * scale);

    const throttleW = isLandscape ? lThrottleW : pThrottleW;
    const throttleH = isLandscape ? lThrottleH : pThrottleH;
    const rotW = isLandscape ? lRotW : pRotW;
    const rotH = isLandscape ? lRotH : pRotH;

    const throttleMax = throttleH / 2 - stickR - 4;
    const rotMax = rotW / 2 - stickR - 4;

    // ---- 油门（垂直推） ----
    const handleThrottleMove = useCallback((_x: number, clientY: number) => {
        if (!thrEl.current) return;
        const {top, height} = thrEl.current.getBoundingClientRect();
        const dist = Math.max(-throttleMax, Math.min(throttleMax, clientY - (top + height / 2)));
        const v = Math.round((-dist / throttleMax) * 100);
        throttleRef.current = v; setThrottle(v);
        if (v !== 0 && !thrActive.current) { thrActive.current = true; startMotor(); }
    }, [throttleMax, startMotor]);

    const handleThrottleEnd = useCallback(() => { throttleRef.current = 0; thrActive.current = false; setThrottle(0); }, []);

    // ---- 方向（水平推） ----
    const handleRotationMove = useCallback((clientX: number, _y: number) => {
        if (!rotEl.current) return;
        const {left, width} = rotEl.current.getBoundingClientRect();
        const dist = Math.max(-rotMax, Math.min(rotMax, clientX - (left + width / 2)));
        const v = Math.round((dist / rotMax) * 100);
        rotationRef.current = v; setRotation(v);
        if (v !== 0 && !rotActive.current) { rotActive.current = true; startMotor(); }
    }, [rotMax, startMotor]);

    const handleRotationEnd = useCallback(() => { rotationRef.current = 0; rotActive.current = false; setRotation(0); }, []);

    // ---- 滑块条渲染 ----
    const renderSlider = (
        ref: React.RefObject<HTMLDivElement | null>,
        vert: boolean, w: number, h: number,
        val: number, color: string,
        onMove: (cx: number, cy: number) => void, onEnd: () => void,
    ) => {
        const md = ((vert ? h : w) / 2 - stickR - 4);
        const pct = md > 0 ? (val / 100) * md : 0;
        const active = Math.abs(val) > 5;

        return (
            <div
                ref={ref}
                onPointerDown={e => { e.currentTarget.setPointerCapture(e.pointerId); onMove(e.clientX, e.clientY); }}
                onPointerMove={e => onMove(e.clientX, e.clientY)}
                onPointerUp={onEnd} onPointerLeave={onEnd} onPointerCancel={onEnd}
                style={{
                    width: w, height: h,
                    borderRadius: scalePx(20),
                    background: "var(--color-bg-card)",
                    border: `2px solid ${active ? color : "var(--color-border)"}`,
                    position: "relative", touchAction: "none", cursor: "pointer",
                    flexShrink: 0, transition: "border-color 0.2s",
                }}
            >
                {/* 中心线 */}
                <div style={{
                    position: "absolute",
                    left: vert ? "50%" : 0, top: vert ? 0 : "50%",
                    transform: vert ? "translateX(-50%)" : "translateY(-50%)",
                    width: vert ? 2 : "100%", height: vert ? "100%" : 2,
                    background: "var(--color-border-light)",
                }} />
                {/* 滑块 */}
                <div style={{
                    position: "absolute",
                    width: stickR * 2, height: stickR * 2, borderRadius: "50%",
                    background: active ? color : "var(--color-bg-elevated)",
                    border: `2px solid ${active ? color : "var(--color-border)"}`,
                    left: vert ? "50%" : `calc(50% + ${pct}px)`,
                    top: vert ? `calc(50% - ${pct}px)` : "50%",
                    transform: "translate(-50%, -50%)",
                    boxShadow: active ? `0 0 10px ${color}55` : "0 2px 6px rgba(0,0,0,0.3)",
                    transition: val === 0 ? "all 0.3s ease" : "none",
                }} />
            </div>
        );
    };

    // ---- 视频 ----
    const videoW = isLandscape
        ? Math.min(window.innerWidth * 0.4, Math.round(340 * scale))
        : Math.min(window.innerWidth - 20, Math.round(340 * scale));
    const videoH = Math.round(videoW * 0.5625);

    const CameraBar = () => (
        <div style={{...S.row, gap: scalePx(6)}}>
            <CameraToggle onStatusChange={setCameraOn} />
            {cameraOn && (
                <button onClick={() => setFpsIndex(i => (i + 1) % 3)} style={{
                    background: fpsIndex > 0 ? "var(--color-success)" : "var(--color-bg-elevated)",
                    border: `1px solid ${fpsIndex > 0 ? "var(--color-success)" : "var(--color-border)"}`,
                    color: fpsIndex > 0 ? "#fff" : "var(--color-text-muted)",
                    padding: `${scalePx(2)} ${scalePx(6)}`, borderRadius: "var(--radius-sm)",
                    fontSize: scalePx(9), fontWeight: 600, cursor: "pointer", outline: "none",
                }}>
                    {FPS_LABELS[fpsIndex]}
                </button>
            )}
        </div>
    );

    const VideoBox = () => (
        <div style={{borderRadius: "var(--radius-md)", overflow: "hidden", border: "2px solid var(--color-border)", flexShrink: 0}}>
            {cameraOn ? (
                <img src={`/api/camera/stream?fps=${FPS_OPTIONS[fpsIndex]}`} alt="Camera"
                     style={{width: videoW, height: videoH, objectFit: "cover", background: "#000", display: "block"}} />
            ) : (
                <div style={{width: videoW, height: videoH, background: "var(--color-bg-card)", display: "flex", alignItems: "center", justifyContent: "center", color: "var(--color-text-muted)", fontSize: scalePx(12)}}>
                    摄像头关闭
                </div>
            )}
        </div>
    );

    const SpeedBar = () => (
        <div style={{...S.row, gap: scalePx(12), fontSize: scalePx(10)}}>
            <span>左 <b style={S.success}>{leftSpeed >= 0 ? "+" : ""}{leftSpeed.toFixed(1)}</b></span>
            <span>右 <b style={S.success}>{rightSpeed >= 0 ? "+" : ""}{rightSpeed.toFixed(1)}</b></span>
            <span style={S.dot(wsConnected)} />
        </div>
    );

    const btnGrab: React.CSSProperties = {
        flex: 1, padding: "16px 0", fontSize: 18, fontWeight: 700,
        background: "linear-gradient(135deg, #16a34a, #22c55e)",
        border: "none", color: "#fff", borderRadius: "var(--radius-md)",
        letterSpacing: "2px", boxShadow: "0 4px 14px rgba(34,197,94,0.35)",
        cursor: "pointer", outline: "none", flexShrink: 0,
    };
    const btnRelease: React.CSSProperties = {
        ...btnGrab,
        background: "linear-gradient(135deg, #2563eb, #3b82f6)",
        boxShadow: "0 4px 14px rgba(59,130,246,0.35)",
    };
    const btnBrake: React.CSSProperties = {
        width: "100%", padding: "10px 0", fontSize: 14, fontWeight: 700,
        background: "var(--color-danger-soft)",
        border: "2px solid var(--color-danger)", color: "var(--color-danger)",
        borderRadius: "var(--radius-md)", letterSpacing: "2px",
        cursor: "pointer", outline: "none", flexShrink: 0,
    };

    // ---- 横屏 ----
    if (isLandscape) {
        return (
            <Page center padTop={4}>
                <div style={{display: "flex", alignItems: "center", justifyContent: "center", gap: scalePx(8), minHeight: `calc(100dvh - var(--tab-bar-height) - 16px)`}}>
                    {/* 左：油门（垂直条） */}
                    <div style={{...S.col, gap: scalePx(4)}}>
                        <span style={{fontSize: scalePx(9), color: "var(--color-text-dim)", letterSpacing: "1px"}}>油门</span>
                        {renderSlider(thrEl, true, throttleW, throttleH, throttle, "var(--color-success)", handleThrottleMove, handleThrottleEnd)}
                        <span style={{fontSize: scalePx(10), fontWeight: 600, color: Math.abs(throttle) > 5 ? "var(--color-success)" : "var(--color-text-muted)"}}>
                            {throttle > 5 ? "▲" : throttle < -5 ? "▼" : "·"} {Math.abs(throttle)}%
                        </span>
                    </div>

                    {/* 中：视频 + 按钮 */}
                    <div style={{...S.col, gap: scalePx(8), flex: 1}}>
                        <CameraBar />
                        <VideoBox />
                        <SpeedBar />
                        <div style={{display: "flex", gap: scalePx(10), flexShrink: 0}}>
                            <button onClick={() => handleArm("grab")} style={btnGrab}>✋ 抓取</button>
                            <button onClick={() => handleArm("release")} style={btnRelease}>🤚 释放</button>
                        </div>
                        <button onClick={handleBrake} style={btnBrake}>⏹ 刹车</button>
                    </div>

                    {/* 右：方向（水平条） */}
                    <div style={{...S.col, gap: scalePx(4)}}>
                        <span style={{fontSize: scalePx(9), color: "var(--color-text-dim)", letterSpacing: "1px"}}>方向</span>
                        {renderSlider(rotEl, false, rotW, rotH, rotation, "var(--color-primary)", handleRotationMove, handleRotationEnd)}
                        <span style={{fontSize: scalePx(10), fontWeight: 600, color: Math.abs(rotation) > 5 ? "var(--color-primary)" : "var(--color-text-muted)"}}>
                            {rotation > 5 ? "→" : rotation < -5 ? "←" : "·"} {Math.abs(rotation)}%
                        </span>
                    </div>
                </div>
            </Page>
        );
    }

    // ---- 竖屏 ----
    return (
        <Page center padTop={6}>
            <div style={{...S.col, gap: scalePx(8)}}>
                <div style={{display: "flex", justifyContent: "space-between", alignItems: "center", width: "100%", maxWidth: videoW}}>
                    <h3 style={{fontSize: scalePx(14), fontWeight: 600, margin: 0}}>摇杆驾驶</h3>
                    <CameraBar />
                </div>
                <VideoBox />
                <SpeedBar />

                {/* 按钮 — 居中 */}
                <div style={{display: "flex", gap: scalePx(12), width: "100%", maxWidth: videoW}}>
                    <button onClick={() => handleArm("grab")} style={btnGrab}>✋ 抓取</button>
                    <button onClick={() => handleArm("release")} style={btnRelease}>🤚 释放</button>
                </div>
                <button onClick={handleBrake} style={{...btnBrake, maxWidth: videoW}}>⏹ 刹车</button>

                {/* 控制条并排 */}
                <div style={{display: "flex", gap: scalePx(12), justifyContent: "center", alignItems: "flex-start"}}>
                    <div style={{...S.col, gap: scalePx(4)}}>
                        <span style={{fontSize: scalePx(9), color: "var(--color-text-dim)", letterSpacing: "1px"}}>油门</span>
                        {renderSlider(thrEl, true, throttleW, throttleH, throttle, "var(--color-success)", handleThrottleMove, handleThrottleEnd)}
                        <span style={{fontSize: scalePx(10), fontWeight: 600, color: Math.abs(throttle) > 5 ? "var(--color-success)" : "var(--color-text-muted)"}}>
                            {throttle > 5 ? "▲" : throttle < -5 ? "▼" : "·"} {Math.abs(throttle)}%
                        </span>
                    </div>
                    <div style={{...S.col, gap: scalePx(4)}}>
                        <span style={{fontSize: scalePx(9), color: "var(--color-text-dim)", letterSpacing: "1px"}}>方向</span>
                        {renderSlider(rotEl, false, rotW, rotH, rotation, "var(--color-primary)", handleRotationMove, handleRotationEnd)}
                        <span style={{fontSize: scalePx(10), fontWeight: 600, color: Math.abs(rotation) > 5 ? "var(--color-primary)" : "var(--color-text-muted)"}}>
                            {rotation > 5 ? "→" : rotation < -5 ? "←" : "·"} {Math.abs(rotation)}%
                        </span>
                    </div>
                </div>
            </div>
        </Page>
    );
};

export default RCDemoPage;
