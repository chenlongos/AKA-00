import {useCallback, useEffect, useRef, useState} from "react";
import {api, controlSocket} from "../api";
import CameraToggle from "../components/CameraToggle";
import {useViewportScale} from "../hooks/useViewportScale";


const RCDemoPage = () => {
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [throttleDisplay, setThrottleDisplay] = useState(0);
    const [directionDisplay, setDirectionDisplay] = useState(0);
    const [isNarrow, setIsNarrow] = useState(false);
    const FPS_OPTIONS = [30, 20, 10];
    const FPS_LABELS = ["高清", "均衡", "省流"];
    const [cameraOn, setCameraOn] = useState(false);
    const [fpsIndex, setFpsIndex] = useState(0);
    const [wsConnected, setWsConnected] = useState(false);

    useEffect(() => {
        controlSocket.connect(setWsConnected, (s) => {
            setLeftSpeed(s.left);
            setRightSpeed(s.right);
        });
        return () => { controlSocket.close(); };
    }, []);

    useEffect(() => {
        const check = () => setIsNarrow(window.innerWidth < window.innerHeight);
        check();
        window.addEventListener("resize", check);
        return () => window.removeEventListener("resize", check);
    }, []);

    const throttleRef = useRef(0);
    const directionRef = useRef(0);
    const motorIntervalRef = useRef<number | null>(null);
    const lastCommandRef = useRef<{x: number, y: number} | null>(null);

    const sendCommand = useCallback(() => {
        const thr = throttleRef.current;
        const dir = directionRef.current;

        const deadZone = 3;
        const thrZero = Math.abs(thr) < deadZone ? 0 : thr;
        const dirZero = Math.abs(dir) < deadZone ? 0 : dir;

        if (lastCommandRef.current &&
            Math.abs(lastCommandRef.current.x - dirZero) < 5 &&
            Math.abs(lastCommandRef.current.y - thrZero) < 5) {
            return;
        }
        lastCommandRef.current = {x: dirZero, y: thrZero};

        controlSocket.sendJoystick(Math.round(dirZero * 0.5), thrZero);
    }, []);

    const startControl = useCallback(() => {
        if (motorIntervalRef.current !== null) return;
        sendCommand();
        motorIntervalRef.current = window.setInterval(sendCommand, 30);
    }, [sendCommand]);

    const stopControl = useCallback(() => {
        if (motorIntervalRef.current !== null) {
            clearInterval(motorIntervalRef.current);
            motorIntervalRef.current = null;
        }
        controlSocket.sendJoystick(0, 0);
        setLeftSpeed(0);
        setRightSpeed(0);
    }, []);

    const {scale, scaleValue, scalePx} = useViewportScale();
    const stickSize = scaleValue(42);
    const baseThrottleW = isNarrow ? 140 : 56;
    const baseThrottleH = isNarrow ? 56 : 140;
    const throttleW = Math.round(baseThrottleW * scale);
    const throttleH = Math.round(baseThrottleH * scale);
    const baseDirectionW = isNarrow ? 56 : 130;
    const baseDirectionH = isNarrow ? 130 : 56;
    const directionW = Math.round(baseDirectionW * scale);
    const directionH = Math.round(baseDirectionH * scale);
    const videoW = isNarrow
        ? Math.min(window.innerWidth - 60, scaleValue(500))
        : Math.min(window.innerWidth * 0.45, scaleValue(700));
    const videoH = isNarrow ? videoW * 0.75 : videoW * 0.5625;  // 16:9 比例

    // 摇杆最大行程（轨道尺寸减去滑块半径和边距）
    const throttleMaxDist = (isNarrow ? throttleW : throttleH) / 2 - stickSize / 2 - 4;
    const directionMaxDist = (isNarrow ? directionH : directionW) / 2 - stickSize / 2 - 4;

    const throttleRefEl = useRef<HTMLDivElement>(null);
    const throttleActiveRef = useRef(false);

    const handleThrottleMove = useCallback((clientX: number, clientY: number) => {
        if (!throttleRefEl.current) return;
        const rect = throttleRefEl.current.getBoundingClientRect();
        const margin = stickSize / 2 + 4;
        let dist, maxDist;

        if (isNarrow) {
            const centerX = rect.left + rect.width / 2;
            const dx = clientX - centerX;
            maxDist = rect.width / 2 - margin;
            dist = Math.max(-maxDist, Math.min(maxDist, dx));
            const thr = Math.round((dist / maxDist) * 100);
            throttleRef.current = thr;
            setThrottleDisplay(thr);
        } else {
            const centerY = rect.top + rect.height / 2;
            const dy = clientY - centerY;
            maxDist = rect.height / 2 - margin;
            dist = Math.max(-maxDist, Math.min(maxDist, dy));
            const thr = Math.round((-dist / maxDist) * 100);
            throttleRef.current = thr;
            setThrottleDisplay(thr);
        }

        if (throttleRef.current !== 0 && !throttleActiveRef.current) {
            throttleActiveRef.current = true;
            startControl();
        }
    }, [isNarrow, startControl, stickSize]);

    const handleThrottleEnd = useCallback(() => {
        throttleRef.current = 0;
        setThrottleDisplay(0);
    }, []);

    const directionRefEl = useRef<HTMLDivElement>(null);
    const directionActiveRef = useRef(false);

    const handleDirectionMove = useCallback((clientX: number, clientY: number) => {
        if (!directionRefEl.current) return;
        const rect = directionRefEl.current.getBoundingClientRect();
        const margin = stickSize / 2 + 4;
        let dist, maxDist;

        if (isNarrow) {
            const centerY = rect.top + rect.height / 2;
            const dy = clientY - centerY;
            maxDist = rect.height / 2 - margin;
            dist = Math.max(-maxDist, Math.min(maxDist, dy));
            const dir = Math.round((dist / maxDist) * 100);
            directionRef.current = dir;
            setDirectionDisplay(dir);
        } else {
            const centerX = rect.left + rect.width / 2;
            const dx = clientX - centerX;
            maxDist = rect.width / 2 - margin;
            dist = Math.max(-maxDist, Math.min(maxDist, dx));
            const dir = Math.round((dist / maxDist) * 100);
            directionRef.current = dir;
            setDirectionDisplay(dir);
        }

        if (directionRef.current !== 0 && !directionActiveRef.current) {
            directionActiveRef.current = true;
            startControl();
        }
    }, [isNarrow, startControl, stickSize]);

    const handleDirectionEnd = useCallback(() => {
        directionRef.current = 0;
        setDirectionDisplay(0);
    }, []);

    const handleArm = async (action: "grab" | "release") => {
        try {
            await api.motor.action(action);
        } catch (err) {
            console.error("机械臂操作失败:", err);
        }
    };

    const handleBack = () => {
        stopControl();
        controlSocket.close();
        navigator.sendBeacon("/api/camera/close");
        window.location.href = "/";
    };

    const handleBrake = () => {
        throttleRef.current = 0;
        directionRef.current = 0;
        setThrottleDisplay(0);
        setDirectionDisplay(0);
        throttleActiveRef.current = false;
        directionActiveRef.current = false;
        stopControl();
    };
    return (
        <div
            style={{
                fontFamily: "system-ui, sans-serif",
                background: "#0f172a",
                color: "white",
                minHeight: "100dvh",
                maxWidth: "100vw",
                padding: scalePx(6),
                paddingBottom: "calc(env(safe-area-inset-bottom) + 40px)",
                textAlign: "center",
                userSelect: "none",
                touchAction: "none",
                display: "flex",
                flexDirection: isNarrow ? "column" : "row",
                alignItems: "center",
                justifyContent: "center",
                gap: isNarrow ? scalePx(8) : scalePx(12),
                overflow: "hidden",
                boxSizing: "border-box",
            }}
        >
            <div style={{display: "flex", flexDirection: "column", alignItems: "center", flex: 1}}>
                <div style={{fontSize: scalePx(12), opacity: 0.6, marginBottom: scalePx(8), transform: isNarrow ? "rotate(90deg)" : "none"}}>油门</div>
                <div
                    ref={throttleRefEl}
                    onPointerDown={(e) => { e.currentTarget.setPointerCapture(e.pointerId); handleThrottleMove(e.clientX, e.clientY); }}
                    onPointerMove={(e) => handleThrottleMove(e.clientX, e.clientY)}
                    onPointerUp={handleThrottleEnd}
                    onPointerLeave={handleThrottleEnd}
                    style={{
                        width: `${throttleW}px`,
                        height: `${throttleH}px`,
                        borderRadius: isNarrow ? scalePx(35) : scalePx(40),
                        background: "rgba(255,255,255,0.1)",
                        border: "2px solid #334155",
                        position: "relative",
                        touchAction: "none",
                    }}
                >
                    <div style={{
                        position: "absolute",
                        left: "50%",
                        top: "50%",
                        transform: "translate(-50%, -50%)",
                        width: isNarrow ? "2px" : "30px",
                        height: isNarrow ? "30px" : "2px",
                        background: "#475569",
                    }}/>
                    <div style={{
                        position: "absolute",
                        width: `${stickSize}px`,
                        height: `${stickSize}px`,
                        borderRadius: "50%",
                        background: throttleDisplay >= 0 ? "#22c55e" : "#ef4444",
                        border: "3px solid #64748b",
                        left: "50%",
                        top: "50%",
                        transform: isNarrow
                            ? `translate(calc(-50% + ${throttleDisplay * throttleMaxDist / 100}px), -50%)`
                            : `translate(-50%, calc(-50% - ${throttleDisplay * throttleMaxDist / 100}px))`,
                        boxShadow: "0 2px 8px rgba(0,0,0,0.3)",
                    }}/>
                </div>
                <div style={{fontSize: scalePx(12), marginTop: scalePx(8), opacity: 0.6, transform: isNarrow ? "rotate(90deg)" : "none"}}>
                    {throttleDisplay > 0 ? "▲" : throttleDisplay < 0 ? "▼" : "·"} {Math.abs(throttleDisplay)}%
                </div>
            </div>

            {/* 中间区域 */}
            <div style={{
                display: "flex",
                flexDirection: "column",
                alignItems: "center",
                flex: 2,
                transform: isNarrow ? "rotate(90deg)" : "none",
                transition: "transform 0.2s",
                position: "relative",
            }}>
                {/* 摄像头开关 + 低带宽模式 */}
                <div style={{position: "absolute", top: "0", right: "0", display: "flex", alignItems: "center", gap: scalePx(6)}}>
                    {cameraOn && (
                        <button
                            onClick={() => setFpsIndex((fpsIndex + 1) % 3)}
                            style={{
                                background: fpsIndex > 0 ? "rgba(34,197,94,0.2)" : "rgba(255,255,255,0.1)",
                                border: `1px solid ${fpsIndex > 0 ? "#22c55e" : "#334155"}`,
                                color: "white",
                                padding: `${scalePx(2)} ${scalePx(8)}`,
                                borderRadius: scalePx(4),
                                fontSize: scalePx(10),
                                cursor: "pointer",
                            }}
                        >
                            {FPS_LABELS[fpsIndex]}
                        </button>
                    )}
                    <span style={{fontSize: scalePx(11), opacity: 0.6}}>摄像头</span>
                    <CameraToggle onStatusChange={setCameraOn} />
                </div>

                {/* 视频流 */}
                <div
                    style={{
                        borderRadius: scalePx(12),
                        overflow: "hidden",
                        border: "2px solid #334155",
                    }}
                >
                    {cameraOn ? (
                        <img
                            src={`/api/camera/stream?fps=${FPS_OPTIONS[fpsIndex]}`}
                            alt="Camera"
                            style={{
                                width: `${videoW}px`,
                                height: `${videoH}px`,
                                objectFit: "cover",
                                background: "#000",
                            }}
                        />
                    ) : (
                        <div
                            style={{
                                width: `${videoW}px`,
                                height: `${videoH}px`,
                                background: "#1e293b",
                                display: "flex",
                                alignItems: "center",
                                justifyContent: "center",
                                color: "#94a3b8",
                                fontSize: scalePx(14),
                            }}
                        >
                            摄像头已关闭
                        </div>
                    )}
                </div>


                {/* 速度显示 + WS状态 */}
                <div style={{display: "flex", justifyContent: "center", alignItems: "center", gap: scalePx(16), fontSize: scalePx(13), marginTop: scalePx(6)}}>
                    <div>左 <span style={{color: "#4ade80", fontWeight: "bold"}}>{leftSpeed > 0 ? "+" : ""}{leftSpeed.toFixed(2)}</span></div>
                    <div>右 <span style={{color: "#4ade80", fontWeight: "bold"}}>{rightSpeed > 0 ? "+" : ""}{rightSpeed.toFixed(2)}</span></div>
                    <span style={{
                        width: "6px", height: "6px", borderRadius: "50%",
                        background: wsConnected ? "#22c55e" : "#ef4444",
                        display: "inline-block",
                    }}/>
                </div>

                {/* 刹车 + 抓取/释放 紧凑排列 */}
                <div style={{display: "flex", flexDirection: "column", gap: scalePx(6), marginTop: scalePx(6), width: "100%", maxWidth: `${videoW}px`}}>
                    <button
                        onClick={handleBrake}
                        style={{
                            width: "100%",
                            background: "rgba(239,68,68,0.25)",
                            border: "2px solid #ef4444",
                            color: "#fca5a5",
                            padding: `${scalePx(8)} ${scalePx(16)}`,
                            borderRadius: scalePx(8),
                            fontSize: `${Math.round(13 * scale)}px`,
                            fontWeight: 600,
                            cursor: "pointer",
                            letterSpacing: "1px",
                        }}
                    >
                        ⏹ 刹车
                    </button>
                    <div style={{display: "flex", gap: scalePx(8)}}>
                        <button
                            onClick={() => handleArm("grab")}
                            style={{
                                flex: 1,
                                background: "linear-gradient(135deg, #16a34a, #22c55e)",
                                border: "none",
                                color: "white",
                                padding: `${scalePx(10)} ${scalePx(16)}`,
                                borderRadius: scalePx(10),
                                fontSize: `${Math.round(15 * scale)}px`,
                                fontWeight: 700,
                                cursor: "pointer",
                                letterSpacing: "1px",
                                boxShadow: "0 3px 8px rgba(34,197,94,0.35)",
                            }}
                        >
                            ✋ 抓取
                        </button>
                        <button
                            onClick={() => handleArm("release")}
                            style={{
                                flex: 1,
                                background: "linear-gradient(135deg, #2563eb, #3b82f6)",
                                border: "none",
                                color: "white",
                                padding: `${scalePx(10)} ${scalePx(16)}`,
                                borderRadius: scalePx(10),
                                fontSize: `${Math.round(15 * scale)}px`,
                                fontWeight: 700,
                                cursor: "pointer",
                                letterSpacing: "1px",
                                boxShadow: "0 3px 8px rgba(59,130,246,0.35)",
                            }}
                        >
                            🤚 释放
                        </button>
                    </div>
                </div>

                {/* 返回按钮 */}
                <button
                    onClick={handleBack}
                    style={{
                        marginTop: scalePx(6),
                        background: "rgba(255,255,255,0.1)",
                        border: "1px solid #334155",
                        color: "white",
                        padding: `${scalePx(5)} ${scalePx(14)}`,
                        borderRadius: scalePx(5),
                        fontSize: scalePx(12),
                        cursor: "pointer",
                    }}
                >
                    返回
                </button>
            </div>

            {/* 方向摇杆 */}
            <div style={{display: "flex", flexDirection: "column", alignItems: "center", flex: 1}}>
                <div style={{fontSize: scalePx(12), opacity: 0.6, marginBottom: "8px", transform: isNarrow ? "rotate(90deg)" : "none"}}>方向</div>
                <div
                    ref={directionRefEl}
                    onPointerDown={(e) => { e.currentTarget.setPointerCapture(e.pointerId); handleDirectionMove(e.clientX, e.clientY); }}
                    onPointerMove={(e) => handleDirectionMove(e.clientX, e.clientY)}
                    onPointerUp={handleDirectionEnd}
                    onPointerLeave={handleDirectionEnd}
                    style={{
                        width: `${directionW}px`,
                        height: `${directionH}px`,
                        borderRadius: isNarrow ? scalePx(35) : scalePx(40),
                        background: "rgba(255,255,255,0.1)",
                        border: "2px solid #334155",
                        position: "relative",
                        touchAction: "none",
                    }}
                >
                    <div style={{
                        position: "absolute",
                        left: "50%",
                        top: "50%",
                        transform: "translate(-50%, -50%)",
                        width: isNarrow ? "30px" : "2px",
                        height: isNarrow ? "2px" : "30px",
                        background: "#475569",
                    }}/>
                    <div style={{
                        position: "absolute",
                        width: `${stickSize}px`,
                        height: `${stickSize}px`,
                        borderRadius: "50%",
                        background: directionDisplay === 0 ? "#475569" : "#2563eb",
                        border: "3px solid #64748b",
                        left: "50%",
                        top: "50%",
                        transform: isNarrow
                            ? `translate(-50%, calc(-50% + ${directionDisplay * directionMaxDist / 100}px))`
                            : `translate(calc(-50% + ${directionDisplay * directionMaxDist / 100}px), -50%)`,
                        boxShadow: "0 2px 8px rgba(0,0,0,0.3)",
                    }}/>
                </div>
                <div style={{fontSize: scalePx(12), marginTop: scalePx(8), opacity: 0.6, transform: isNarrow ? "rotate(90deg)" : "none"}}>
                    {directionDisplay > 0 ? "→" : directionDisplay < 0 ? "←" : "·"} {Math.abs(directionDisplay)}%
                </div>
            </div>
        </div>
    );
};

export default RCDemoPage;