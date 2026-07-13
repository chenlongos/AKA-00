import {useCallback, useEffect, useRef, useState} from "react";
import {api, controlSocket} from "../api";
import CameraToggle from "../components/CameraToggle";
import FullscreenButton from "../components/FullscreenButton";
import TabBar from "../components/TabBar";
import Page from "../components/Page";
import {S} from "../styles";
import {useViewportScale} from "../hooks/useViewportScale";

const FPS_OPTIONS = [30, 20, 10];
const FPS_LABELS = ["高清", "均衡", "省流"];

const CameraBar = ({
    scalePx, cameraOn, fpsIndex, frameReady, onStatusChange, onFpsChange, syncKey,
}: {
    scalePx: (px: number) => string;
    cameraOn: boolean;
    fpsIndex: number;
    frameReady?: boolean;
    onStatusChange: (on: boolean) => void;
    onFpsChange: () => void;
    syncKey: number;
}) => (
    <div style={{display: "flex", alignItems: "center", gap: scalePx(6)}}>
        <CameraToggle key={syncKey} onStatusChange={onStatusChange} frameReady={frameReady} />
        {cameraOn && (
            <button onClick={onFpsChange} style={{
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

const RCDemoPage = () => {
    const {scalePx, scale} = useViewportScale();
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [throttle, setThrottle] = useState(0);
    const [rotation, setRotation] = useState(0);
    const [cameraOn, setCameraOn] = useState(false);
    const [cameraSyncKey, setCameraSyncKey] = useState(0);
    // MJPEG <img> 首帧到达信号：传给 CameraToggle 让 loading spinner 持续到
    // camera-node 真正有 JPEG 推过来，不只是 HTTP 成功。
    const [frameReady, setFrameReady] = useState(false);
    const [fpsIndex, setFpsIndex] = useState(0);
    const [wsConnected, setWsConnected] = useState(false);
    const [isLandscape, setIsLandscape] = useState(() => window.innerWidth > window.innerHeight);
    const [bottomOpen, setBottomOpen] = useState(false);

    const showBottom = useCallback(() => {
        setBottomOpen(true);
    }, []);

    const hideBottom = useCallback(() => {
        setBottomOpen(false);
        window.dispatchEvent(new CustomEvent("toggle-tabbar", {detail: {show: false}}));
    }, []);

    const throttleRef = useRef(0);
    const rotationRef = useRef(0);
    const intervalRef = useRef<number | null>(null);
    const lastCmdRef = useRef<{x: number, y: number} | null>(null);
    const thrEl = useRef<HTMLDivElement>(null);
    const thrActive = useRef(false);
    const rotEl = useRef<HTMLDivElement>(null);
    const rotActive = useRef(false);
    const padEl = useRef<HTMLDivElement>(null);
    const padActiveRef = useRef(false);

    useEffect(() => {
        const check = () => setIsLandscape(window.innerWidth > window.innerHeight);
        window.addEventListener("resize", check); window.addEventListener("orientationchange", check);
        return () => { window.removeEventListener("resize", check); window.removeEventListener("orientationchange", check); };
    }, []);

    // 每次摄像头重新打开，把 frameReady 清成 false 等首帧；
    // img.onLoad 把它拉到 true → CameraToggle 据此关 loading。
    useEffect(() => {
        if (cameraOn) setFrameReady(false);
    }, [cameraOn]);

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
        return () => { stopMotor(); controlSocket.close(); };
    }, []);

    // 横屏默认隐藏 tab bar，竖屏显示
    useEffect(() => {
        if (isLandscape) {
            hideBottom();
        } else {
            window.dispatchEvent(new CustomEvent("toggle-tabbar", {detail: {show: true}}));
        }
    }, [isLandscape, hideBottom]);

    const handleBrake = () => {
        throttleRef.current = 0; rotationRef.current = 0;
        setThrottle(0); setRotation(0);
        thrActive.current = false; rotActive.current = false;
        stopMotor();
    };

    const handleArm = (a: "grab" | "release") => { controlSocket.sendAction(a); };

    // ====== 尺寸 ======
    const vw = window.innerWidth;
    const vh = window.innerHeight;
    const pad = Math.round(8 * scale);
    const gap = Math.round(10 * scale);
    const stickR = Math.round(20 * scale);
    const topH = Math.round(44 * scale);

    // 竖屏：视频上面、控制下面
    const pVideoW = vw - pad * 2;
    const pVideoH = Math.round(pVideoW * 0.5625);

    // 横屏摇杆参数
    const joyW = Math.round(vw * 0.22);
    const joySide = Math.round(vw * 0.04);

    // ---- 摇杆移动（用 ref 避免 useCallback 重建） ----
    const sliderMax = useRef({throttle: 0, rot: 0});
    const handleSliderMove = useCallback((
        clientPos: number, vertical: boolean, el: HTMLDivElement | null,
        setter: (v: number) => void, valRef: React.MutableRefObject<number>,
        activeRef: React.MutableRefObject<boolean>,
    ) => {
        if (!el) return;
        const rect = el.getBoundingClientRect();
        const length = vertical ? rect.height : rect.width;
        const mid = vertical ? rect.top + length / 2 : rect.left + length / 2;
        const maxDist = length / 2 - stickR - 4;
        sliderMax.current = {throttle: maxDist, rot: maxDist};
        const dist = Math.max(-maxDist, Math.min(maxDist, clientPos - mid));
        const v = Math.round((vertical ? -dist : dist) / maxDist * 100);
        if (Math.abs(v) < 3) { valRef.current = 0; setter(0); return; }
        valRef.current = v; setter(v);
        if (!activeRef.current) { activeRef.current = true; startMotor(); }
    }, [stickR, startMotor]);

    const handleThrottleMove = useCallback((_x: number, clientY: number) => {
        handleSliderMove(clientY, true, thrEl.current, setThrottle, throttleRef, thrActive);
    }, [handleSliderMove]);
    const handleThrottleEnd = useCallback(() => { throttleRef.current = 0; thrActive.current = false; setThrottle(0); }, []);
    const handleRotationMove = useCallback((clientX: number, _y: number) => {
        handleSliderMove(clientX, false, rotEl.current, setRotation, rotationRef, rotActive);
    }, [handleSliderMove]);
    const handleRotationEnd = useCallback(() => { rotationRef.current = 0; rotActive.current = false; setRotation(0); }, []);

    // 竖屏大圆盘：双轴控制
    const handlePadMove = useCallback((clientX: number, clientY: number) => {
        if (!padEl.current) return;
        const rect = padEl.current.getBoundingClientRect();
        const cx = rect.left + rect.width / 2;
        const cy = rect.top + rect.height / 2;
        const maxR = rect.width / 2 - stickR - 4;
        const dx = clientX - cx;
        const dy = clientY - cy;
        const dist = Math.sqrt(dx * dx + dy * dy);
        const clampDist = Math.min(dist, maxR);
        const scale = dist > 0 ? clampDist / dist : 0;
        const rx = Math.round((dx * scale) / maxR * 100);
        const ry = Math.round(-(dy * scale) / maxR * 100);  // 上=正
        rotationRef.current = Math.abs(rx) < 5 ? 0 : rx;
        throttleRef.current = Math.abs(ry) < 5 ? 0 : ry;
        setRotation(rotationRef.current);
        setThrottle(throttleRef.current);
        if (!padActiveRef.current) { padActiveRef.current = true; startMotor(); }
    }, [stickR, startMotor]);

    const handlePadEnd = useCallback(() => {
        throttleRef.current = 0; rotationRef.current = 0;
        setThrottle(0); setRotation(0);
        padActiveRef.current = false;
    }, []);

    // ---- 摇杆渲染 ----
    const renderJoystick = (
        ref: React.RefObject<HTMLDivElement | null>, vert: boolean, w: number, h: number,
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
                    borderRadius: scalePx(16),
                    border: `2px solid ${active ? color : "rgba(255,255,255,0.15)"}`,
                    position: "relative", touchAction: "none", cursor: "pointer", flexShrink: 0,
                    overflow: "hidden",
                }}
            >
                {/* 毛玻璃背景层 — 只改 opacity，不触发 repaint/blur 重算 */}
                <div style={{
                    position: "absolute", inset: 0,
                    background: "rgba(255,255,255,0.12)",
                    backdropFilter: "blur(4px)", WebkitBackdropFilter: "blur(4px)",
                    opacity: active ? 1 : 0.5,
                    transition: "opacity 0.15s ease",
                }}/>
                <div style={{
                    position: "absolute",
                    left: vert ? "50%" : 0, top: vert ? 0 : "50%",
                    transform: vert ? "translateX(-50%)" : "translateY(-50%)",
                    width: vert ? 2 : "100%", height: vert ? "100%" : 2,
                    background: "rgba(255,255,255,0.1)",
                }}/>
                <div style={{
                    position: "absolute",
                    width: stickR * 2, height: stickR * 2, borderRadius: "50%",
                    background: active ? color : "rgba(255,255,255,0.5)",
                    left: vert ? "50%" : `calc(50% + ${pct}px)`,
                    top: vert ? `calc(50% - ${pct}px)` : "50%",
                    transform: "translate(-50%, -50%)",
                    boxShadow: active ? `0 0 16px ${color}88` : "0 2px 8px rgba(0,0,0,0.3)",
                    transition: val === 0 ? "all 0.25s ease" : "none",
                }}/>
            </div>
        );
    };

    // ---- 按钮 ----
    const btnGrab: React.CSSProperties = {
        flex: 1, padding: `${scalePx(14)} 0`, fontSize: scalePx(16), fontWeight: 700,
        background: "linear-gradient(135deg, #16a34a, #22c55e)", border: "none", color: "#fff",
        borderRadius: "var(--radius-md)", letterSpacing: "2px",
        boxShadow: "0 4px 14px rgba(34,197,94,0.35)", cursor: "pointer", outline: "none",
    };
    const btnRelease: React.CSSProperties = {
        ...btnGrab, background: "linear-gradient(135deg, #2563eb, #3b82f6)",
        boxShadow: "0 4px 14px rgba(59,130,246,0.35)",
    };
    const btnBrake: React.CSSProperties = {
        flex: 1, padding: `${scalePx(12)} 0`, fontSize: scalePx(15), fontWeight: 700,
        background: "var(--color-danger-soft)", border: "2px solid var(--color-danger)",
        color: "var(--color-danger)", borderRadius: "var(--radius-md)",
        letterSpacing: "2px", cursor: "pointer", outline: "none",
    };

    // ====== 横屏：视频全屏 + 浮层控件 ======
    if (isLandscape) {
        // 读取实际 safe-area-inset-bottom（iPhone 横条 ~34px，普通设备 0）
        const safeBottomPx = parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--safe-bottom")) || 0;
        const scaledTabBarPx = Math.round(34 * scale);  // 跟随全局 --tab-bar-height
        const tabBarHeight = scaledTabBarPx + safeBottomPx;  // 总高度 = 基础 + 安全区
        const joyAreaBottom = Math.round(12 * scale);
        const navBarOffset = bottomOpen ? tabBarHeight : 0;
        const throttleH = Math.min((vh - joyAreaBottom - navBarOffset) * 0.65, Math.round(300 * scale));
        const rotH = Math.round(64 * scale);
        return (
            <div style={{position: "fixed", inset: 0, background: "#000", overflow: "hidden"}}
                 onPointerDown={hideBottom}>
                {/* 视频全屏 */}
                <div style={{position: "absolute", inset: 0}}>
                    {cameraOn ? (
                        <img src={`/api/camera/stream?fps=${FPS_OPTIONS[fpsIndex]}`}
                             onLoad={() => setFrameReady(true)}
                             style={{width: "100%", height: "100%", objectFit: "cover", display: "block"}} alt="" />
                    ) : (
                        <div
                            onPointerDown={e => { e.stopPropagation(); api.camera.open().then(d => { setCameraOn(d.camera_on); setCameraSyncKey(k => k + 1); }); }}
                            style={{display: "flex", alignItems: "center", justifyContent: "center", height: "100%", color: "var(--color-text-muted)", fontSize: scalePx(16), cursor: "pointer"}}>
                            摄像头关闭 — 点击开启
                        </div>
                    )}
                </div>

                {/* 顶部信息 */}
                <div style={{position: "absolute", top: 0, left: 0, right: 0, zIndex: 2,
                    padding: `${scalePx(6)} ${scalePx(12)}`, display: "flex",
                    justifyContent: "flex-end", alignItems: "center", gap: scalePx(8), pointerEvents: "auto"}}>
                    <FullscreenButton />
                    <div style={{display: "flex", gap: scalePx(10), fontSize: scalePx(11), color: "#fff"}}>
                        <span>左 <b style={{color: "#4ade80"}}>{leftSpeed >= 0 ? "+" : ""}{leftSpeed.toFixed(1)}</b></span>
                        <span>右 <b style={{color: "#4ade80"}}>{rightSpeed >= 0 ? "+" : ""}{rightSpeed.toFixed(1)}</b></span>
                        <span style={{width: 8, height: 8, borderRadius: "50%", background: wsConnected ? "#4ade80" : "#ef4444", alignSelf: "center"}} />
                    </div>
                </div>

                {/* 左下：油门 + 刹车 */}
                <div style={{position: "absolute", left: joySide, bottom: joyAreaBottom + navBarOffset + Math.round(vh * 0.02),
                    zIndex: 2, display: "flex", flexDirection: "column", alignItems: "center", gap: scalePx(6)}}>
                    {renderJoystick(thrEl, true, Math.round(70 * scale), throttleH, throttle, "var(--color-success)", handleThrottleMove, handleThrottleEnd)}
                    <span style={{fontSize: scalePx(10), fontWeight: 600, color: Math.abs(throttle) > 5 ? "var(--color-success)" : "rgba(255,255,255,0.5)"}}>
                        {throttle > 5 ? "▲" : throttle < -5 ? "▼" : "·"} {Math.abs(throttle)}%
                    </span>
                    <button onClick={handleBrake} style={{
                        padding: `${scalePx(4)} ${scalePx(10)}`, fontSize: scalePx(11), fontWeight: 700,
                        background: "#ef4444", border: "none", color: "#fff",
                        borderRadius: scalePx(6), cursor: "pointer", outline: "none",
                        boxShadow: "0 2px 8px rgba(239,68,68,0.4)",
                    }}>⏹ 刹车</button>
                </div>

                {/* 右下：摄像头 + 抓取/释放 + 方向 */}
                <div style={{position: "absolute", right: joySide, bottom: joyAreaBottom + navBarOffset + Math.round(vh * 0.04),
                    zIndex: 2, display: "flex", flexDirection: "column", alignItems: "center", gap: scalePx(6)}}>
                    <div style={{display: "flex", alignItems: "center", gap: scalePx(6), alignSelf: "flex-end"}}>
                        <CameraToggle key={cameraSyncKey} onStatusChange={setCameraOn} frameReady={frameReady} />
                        {cameraOn && (
                            <button onClick={() => setFpsIndex(i => (i + 1) % 3)} style={{
                                background: fpsIndex > 0 ? "var(--color-success)" : "rgba(255,255,255,0.15)",
                                border: "none", color: "#fff", padding: `${scalePx(2)} ${scalePx(6)}`,
                                borderRadius: "var(--radius-sm)", fontSize: scalePx(9), fontWeight: 600, cursor: "pointer",
                            }}>
                                {FPS_LABELS[fpsIndex]}
                            </button>
                        )}
                    </div>
                    <div style={{display: "flex", gap: scalePx(6), width: joyW}}>
                        <button onClick={() => handleArm("grab")} style={{
                            flex: 1, padding: `${scalePx(8)} 0`, fontSize: scalePx(13), fontWeight: 700,
                            background: "linear-gradient(135deg, #16a34a, #22c55e)", border: "none", color: "#fff",
                            borderRadius: scalePx(6), cursor: "pointer", outline: "none",
                            boxShadow: "0 2px 8px rgba(34,197,94,0.3)",
                        }}>✋ 抓取</button>
                        <button onClick={() => handleArm("release")} style={{
                            flex: 1, padding: `${scalePx(8)} 0`, fontSize: scalePx(13), fontWeight: 700,
                            background: "linear-gradient(135deg, #2563eb, #3b82f6)", border: "none", color: "#fff",
                            borderRadius: scalePx(6), cursor: "pointer", outline: "none",
                            boxShadow: "0 2px 8px rgba(59,130,246,0.3)",
                        }}>🤚 释放</button>
                    </div>
                    {renderJoystick(rotEl, false, joyW, rotH, rotation, "var(--color-primary)", handleRotationMove, handleRotationEnd)}
                    <span style={{fontSize: scalePx(10), fontWeight: 600, color: Math.abs(rotation) > 5 ? "var(--color-primary)" : "rgba(255,255,255,0.5)"}}>
                        {rotation > 5 ? "→" : rotation < -5 ? "←" : "·"} {Math.abs(rotation)}%
                    </span>
                </div>

                {/* 底部：折叠按钮 + TabBar */}
                <div style={{position: "absolute", bottom: bottomOpen ? tabBarHeight : 0, left: 0, right: 0, zIndex: 3,
                    display: "flex", justifyContent: "center", transition: "bottom 0.3s ease"}}>
                    <button onPointerDown={e => { e.stopPropagation(); bottomOpen ? hideBottom() : showBottom(); }} style={{
                        padding: `${scalePx(2)} ${scalePx(10)}`, fontSize: scalePx(10),
                        background: "rgba(0,0,0,0.5)", border: "1px solid rgba(255,255,255,0.15)",
                        color: "#fff", borderRadius: `${scalePx(6)} ${scalePx(6)} 0 0`,
                        cursor: "pointer", outline: "none", backdropFilter: "blur(6px)",
                    }}>
                        {bottomOpen ? "▼ 导航" : "▲ 导航"}
                    </button>
                </div>

                {/* 横屏内嵌 TabBar — 覆盖 --tab-bar-height 跟随缩放 */}
                {bottomOpen && (
                    <div
                        onPointerDown={e => e.stopPropagation()}
                        style={{
                        position: "absolute", bottom: 0, left: 0, right: 0, zIndex: 3,
                        "--tab-bar-height": `${scaledTabBarPx}px`,
                    } as React.CSSProperties}>
                        <TabBar />
                    </div>
                )}

            </div>
        );
    }

    // ====== 竖屏：大圆盘摇杆 ======
    const padSize = Math.round(Math.min(pVideoW * 0.65, vh - pVideoH - topH - Math.round(140 * scale)));
    const padKnobR = Math.round(22 * scale);
    const padMaxR = padSize / 2 - padKnobR - 4;
    const padKnobX = padMaxR > 0 ? (rotation / 100) * padMaxR : 0;
    const padKnobY = padMaxR > 0 ? -(throttle / 100) * padMaxR : 0;
    const padActive = Math.abs(throttle) > 5 || Math.abs(rotation) > 5;

    return (
        <Page center padTop={6}>
            <div style={{...S.col, gap, alignItems: "center", width: pVideoW}}>
                {/* 顶部栏 */}
                <div style={{display: "flex", justifyContent: "space-between", alignItems: "center", width: "100%", height: topH, marginTop: "5px"}}>
                    <CameraBar scalePx={scalePx} cameraOn={cameraOn} fpsIndex={fpsIndex} frameReady={frameReady} onStatusChange={setCameraOn} onFpsChange={() => setFpsIndex(i => (i + 1) % 3)} syncKey={cameraSyncKey} />
                    <div style={{display: "flex", gap: scalePx(8), fontSize: scalePx(10), whiteSpace: "nowrap", alignItems: "center"}}>
                        <FullscreenButton />
                        <span>左 <b style={S.success}>{leftSpeed >= 0 ? "+" : ""}{leftSpeed.toFixed(1)}</b></span>
                        <span>右 <b style={S.success}>{rightSpeed >= 0 ? "+" : ""}{rightSpeed.toFixed(1)}</b></span>
                        <span style={S.dot(wsConnected)} />
                    </div>
                </div>

                {/* 视频 */}
                <div style={{borderRadius: "var(--radius-md)", overflow: "hidden", border: "2px solid var(--color-border)", flexShrink: 0}}>
                    {cameraOn ? (
                        <img src={`/api/camera/stream?fps=${FPS_OPTIONS[fpsIndex]}`}
                             onLoad={() => setFrameReady(true)}
                             style={{width: pVideoW, height: pVideoH, objectFit: "cover", background: "#000", display: "block"}} alt="" />
                    ) : (
                        <div style={{width: pVideoW, height: pVideoH, background: "var(--color-bg-card)", display: "flex", alignItems: "center", justifyContent: "center", color: "var(--color-text-muted)", fontSize: scalePx(14)}}>
                            摄像头关闭
                        </div>
                    )}
                </div>

                {/* 大圆盘摇杆 */}
                <div
                    ref={padEl}
                    onPointerDown={e => { e.currentTarget.setPointerCapture(e.pointerId); handlePadMove(e.clientX, e.clientY); }}
                    onPointerMove={e => handlePadMove(e.clientX, e.clientY)}
                    onPointerUp={handlePadEnd} onPointerLeave={handlePadEnd} onPointerCancel={handlePadEnd}
                    style={{
                        width: padSize, height: padSize, borderRadius: "50%",
                        background: padActive ? "rgba(255,255,255,0.08)" : "rgba(255,255,255,0.04)",
                        border: `3px solid ${padActive ? "rgba(255,255,255,0.25)" : "rgba(255,255,255,0.1)"}`,
                        position: "relative", touchAction: "none", cursor: "pointer", flexShrink: 0,
                        transition: "border-color 0.2s, background 0.2s",
                    }}
                >
                    {/* 十字参考线 */}
                    <div style={{position: "absolute", left: "50%", top: 0, transform: "translateX(-50%)", width: 1, height: "100%", background: "rgba(255,255,255,0.06)"}} />
                    <div style={{position: "absolute", top: "50%", left: 0, transform: "translateY(-50%)", height: 1, width: "100%", background: "rgba(255,255,255,0.06)"}} />
                    {/* 摇杆头 */}
                    <div style={{
                        position: "absolute",
                        width: padKnobR * 2, height: padKnobR * 2, borderRadius: "50%",
                        background: padActive ? "var(--color-primary)" : "rgba(255,255,255,0.35)",
                        left: `calc(50% + ${padKnobX}px)`, top: `calc(50% + ${padKnobY}px)`,
                        transform: "translate(-50%, -50%)",
                        boxShadow: padActive ? "0 0 20px rgba(96,165,250,0.5)" : "0 2px 8px rgba(0,0,0,0.3)",
                        transition: (throttle === 0 && rotation === 0) ? "all 0.25s ease" : "none",
                    }} />
                </div>

                {/* 速度指示 */}
                <div style={{display: "flex", gap: scalePx(16), fontSize: scalePx(10)}}>
                    <span>油门 <b style={{color: Math.abs(throttle) > 5 ? "var(--color-success)" : "var(--color-text-muted)"}}>{throttle > 5 ? "▲" : throttle < -5 ? "▼" : "·"} {Math.abs(throttle)}%</b></span>
                    <span>方向 <b style={{color: Math.abs(rotation) > 5 ? "var(--color-primary)" : "var(--color-text-muted)"}}>{rotation > 5 ? "→" : rotation < -5 ? "←" : "·"} {Math.abs(rotation)}%</b></span>
                </div>

                {/* 按钮 */}
                <div style={{display: "flex", gap: scalePx(10), width: "100%"}}>
                    <button onClick={() => handleArm("grab")} style={btnGrab}>✋ 抓取</button>
                    <button onClick={() => handleArm("release")} style={btnRelease}>🤚 释放</button>
                </div>
                <button onClick={handleBrake} style={{...btnBrake, width: "100%"}}>⏹ 刹车</button>
            </div>
        </Page>
    );
};

export default RCDemoPage;