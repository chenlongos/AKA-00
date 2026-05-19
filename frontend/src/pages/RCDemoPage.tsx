import {useCallback, useEffect, useRef, useState} from "react";
import {api} from "../api";
import CameraToggle from "../components/CameraToggle";


const RCDemoPage = () => {
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [throttleDisplay, setThrottleDisplay] = useState(0);
    const [directionDisplay, setDirectionDisplay] = useState(0);
    const [isNarrow, setIsNarrow] = useState(false);
    const [cameraOn, setCameraOn] = useState(false);

    useEffect(() => {
        const check = () => setIsNarrow(window.innerWidth < window.innerHeight);
        check();
        window.addEventListener("resize", check);
        return () => window.removeEventListener("resize", check);
    }, []);

    const throttleRef = useRef(0);
    const directionRef = useRef(0);
    const motorIntervalRef = useRef<number | null>(null);
    const lastCommandRef = useRef<{left: number, right: number} | null>(null);

    const sendCommand = useCallback(async () => {
        const thr = throttleRef.current;
        const dir = directionRef.current;

        const deadZone = 3;
        const thrZero = Math.abs(thr) < deadZone ? 0 : thr;
        const dirZero = Math.abs(dir) < deadZone ? 0 : dir;

        const base = thrZero;
        const turn = dirZero * 0.5;
        const left = Math.max(-100, Math.min(100, base - turn));
        const right = Math.max(-100, Math.min(100, base + turn));

        if (lastCommandRef.current &&
            Math.abs(lastCommandRef.current.left - left) < 5 &&
            Math.abs(lastCommandRef.current.right - right) < 5) {
            return;
        }
        lastCommandRef.current = {left, right};

        try {
            const data = await api.motor.direct(left, right);
            setLeftSpeed(data.left_speed ?? 0);
            setRightSpeed(data.right_speed ?? 0);
        } catch {}
    }, []);

    const startControl = useCallback(() => {
        if (motorIntervalRef.current !== null) return;
        sendCommand();
        motorIntervalRef.current = window.setInterval(sendCommand, 200);
    }, [sendCommand]);

    const stopControl = useCallback(() => {
        if (motorIntervalRef.current !== null) {
            clearInterval(motorIntervalRef.current);
            motorIntervalRef.current = null;
        }
        api.motor.direct(0, 0).catch(() => {});
        setLeftSpeed(0);
        setRightSpeed(0);
    }, []);

    const throttleRefEl = useRef<HTMLDivElement>(null);
    const throttleActiveRef = useRef(false);

    const handleThrottleMove = useCallback((clientX: number, clientY: number) => {
        if (!throttleRefEl.current) return;
        const rect = throttleRefEl.current.getBoundingClientRect();
        let dist, maxDist;

        if (isNarrow) {
            const centerX = rect.left + rect.width / 2;
            const dx = clientX - centerX;
            maxDist = rect.width / 2 - 20;
            dist = Math.max(-maxDist, Math.min(maxDist, dx));
            const thr = Math.round((dist / maxDist) * 100);
            throttleRef.current = thr;
            setThrottleDisplay(thr);
        } else {
            const centerY = rect.top + rect.height / 2;
            const dy = clientY - centerY;
            maxDist = rect.height / 2 - 20;
            dist = Math.max(-maxDist, Math.min(maxDist, dy));
            const thr = Math.round((-dist / maxDist) * 100);
            throttleRef.current = thr;
            setThrottleDisplay(thr);
        }

        if (throttleRef.current !== 0 && !throttleActiveRef.current) {
            throttleActiveRef.current = true;
            startControl();
        }
    }, [isNarrow, startControl]);

    const handleThrottleEnd = useCallback(() => {
        throttleRef.current = 0;
        setThrottleDisplay(0);
    }, []);

    const directionRefEl = useRef<HTMLDivElement>(null);
    const directionActiveRef = useRef(false);

    const handleDirectionMove = useCallback((clientX: number, clientY: number) => {
        if (!directionRefEl.current) return;
        const rect = directionRefEl.current.getBoundingClientRect();
        let dist, maxDist;

        if (isNarrow) {
            const centerY = rect.top + rect.height / 2;
            const dy = clientY - centerY;
            maxDist = rect.height / 2 - 20;
            dist = Math.max(-maxDist, Math.min(maxDist, dy));
            const dir = Math.round((-dist / maxDist) * 100);
            directionRef.current = dir;
            setDirectionDisplay(dir);
        } else {
            const centerX = rect.left + rect.width / 2;
            const dx = clientX - centerX;
            maxDist = rect.width / 2 - 20;
            dist = Math.max(-maxDist, Math.min(maxDist, dx));
            const dir = Math.round((dist / maxDist) * 100);
            directionRef.current = dir;
            setDirectionDisplay(dir);
        }

        if (directionRef.current !== 0 && !directionActiveRef.current) {
            directionActiveRef.current = true;
            startControl();
        }
    }, [isNarrow, startControl]);

    const handleDirectionEnd = useCallback(() => {
        directionRef.current = 0;
        setDirectionDisplay(0);
    }, []);

    const handleBack = () => {
        stopControl();
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

    const stickSize = 55;
    const throttleW = isNarrow ? 160 : 70;
    const throttleH = isNarrow ? 70 : 160;
    const directionW = isNarrow ? 70 : 160;
    const directionH = isNarrow ? 160 : 70;
    const videoW = isNarrow ? Math.min(window.innerWidth - 60, 320) : Math.min(window.innerWidth - 100, 480);
    const videoH = isNarrow ? videoW * 0.75 : videoW * 0.5;

    return (
        <div
            style={{
                fontFamily: "system-ui, sans-serif",
                background: "#0f172a",
                color: "white",
                minHeight: "100dvh",
                padding: "10px",
                paddingBottom: "calc(env(safe-area-inset-bottom) + 40px)",
                textAlign: "center",
                userSelect: "none",
                touchAction: "none",
                display: "flex",
                flexDirection: isNarrow ? "column" : "row",
                alignItems: "center",
                justifyContent: "center",
                gap: isNarrow ? "10px" : "20px",
                overflow: "hidden",
            }}
        >
            <div style={{display: "flex", flexDirection: "column", alignItems: "center", flex: 1}}>
                <div style={{fontSize: "12px", opacity: 0.6, marginBottom: "8px", transform: isNarrow ? "rotate(90deg)" : "none"}}>油门</div>
                <div
                    ref={throttleRefEl}
                    onPointerDown={(e) => { e.currentTarget.setPointerCapture(e.pointerId); handleThrottleMove(e.clientX, e.clientY); }}
                    onPointerMove={(e) => handleThrottleMove(e.clientX, e.clientY)}
                    onPointerUp={handleThrottleEnd}
                    onPointerLeave={handleThrottleEnd}
                    style={{
                        width: `${throttleW}px`,
                        height: `${throttleH}px`,
                        borderRadius: isNarrow ? "35px" : "40px",
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
                            ? `translate(calc(-50% + ${throttleDisplay * 0.5}px), -50%)`
                            : `translate(-50%, calc(-50% - ${throttleDisplay * 0.5}px))`,
                        boxShadow: "0 2px 8px rgba(0,0,0,0.3)",
                    }}/>
                </div>
                <div style={{fontSize: "12px", marginTop: "8px", opacity: 0.6, transform: isNarrow ? "rotate(90deg)" : "none"}}>
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
                {/* 摄像头开关 */}
                <div style={{position: "absolute", top: "0", right: "0", display: "flex", alignItems: "center", gap: "6px"}}>
                    <span style={{fontSize: "11px", opacity: 0.6}}>摄像头</span>
                    <CameraToggle onStatusChange={setCameraOn} />
                </div>

                {/* 视频流 */}
                <div
                    style={{
                        borderRadius: "12px",
                        overflow: "hidden",
                        border: "2px solid #334155",
                    }}
                >
                    {cameraOn ? (
                        <img
                            src="/api/camera/stream"
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
                                fontSize: "14px",
                            }}
                        >
                            摄像头已关闭
                        </div>
                    )}
                </div>


                {/* 速度显示 */}
                <div style={{display: "flex", justifyContent: "center", gap: "20px", fontSize: "13px", marginTop: "10px"}}>
                    <div>左 <span style={{color: "#4ade80", fontWeight: "bold"}}>{leftSpeed > 0 ? "+" : ""}{leftSpeed.toFixed(2)}</span></div>
                    <div>右 <span style={{color: "#4ade80", fontWeight: "bold"}}>{rightSpeed > 0 ? "+" : ""}{rightSpeed.toFixed(2)}</span></div>
                </div>

                {/* 刹车按钮 */}
                <button
                    onClick={handleBrake}
                    style={{
                        marginTop: "10px",
                        background: "rgba(239,68,68,0.3)",
                        border: "1px solid #ef4444",
                        color: "white",
                        padding: "6px 16px",
                        borderRadius: "6px",
                        fontSize: "12px",
                        cursor: "pointer",
                    }}
                >
                    刹车
                </button>

                {/* 返回按钮 */}
                <button
                    onClick={handleBack}
                    style={{
                        marginTop: "10px",
                        marginLeft: "10px",
                        background: "rgba(255,255,255,0.1)",
                        border: "1px solid #334155",
                        color: "white",
                        padding: "6px 16px",
                        borderRadius: "6px",
                        fontSize: "12px",
                        cursor: "pointer",
                    }}
                >
                    返回
                </button>
            </div>

            {/* 方向摇杆 */}
            <div style={{display: "flex", flexDirection: "column", alignItems: "center", flex: 1}}>
                <div style={{fontSize: "12px", opacity: 0.6, marginBottom: "8px", transform: isNarrow ? "rotate(90deg)" : "none"}}>方向</div>
                <div
                    ref={directionRefEl}
                    onPointerDown={(e) => { e.currentTarget.setPointerCapture(e.pointerId); handleDirectionMove(e.clientX, e.clientY); }}
                    onPointerMove={(e) => handleDirectionMove(e.clientX, e.clientY)}
                    onPointerUp={handleDirectionEnd}
                    onPointerLeave={handleDirectionEnd}
                    style={{
                        width: `${directionW}px`,
                        height: `${directionH}px`,
                        borderRadius: isNarrow ? "35px" : "40px",
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
                            ? `translate(-50%, calc(-50% - ${directionDisplay * 0.5}px))`
                            : `translate(calc(-50% + ${directionDisplay * 0.5}px), -50%)`,
                        boxShadow: "0 2px 8px rgba(0,0,0,0.3)",
                    }}/>
                </div>
                <div style={{fontSize: "12px", marginTop: "8px", opacity: 0.6, transform: isNarrow ? "rotate(90deg)" : "none"}}>
                    {directionDisplay > 0 ? "→" : directionDisplay < 0 ? "←" : "·"} {Math.abs(directionDisplay)}%
                </div>
            </div>
        </div>
    );
};

export default RCDemoPage;