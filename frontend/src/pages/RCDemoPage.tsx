import {useCallback, useEffect, useRef, useState} from "react";


const RCDemoPage = () => {
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [throttleDisplay, setThrottleDisplay] = useState(0);
    const [directionDisplay, setDirectionDisplay] = useState(0);
    const [isNarrow, setIsNarrow] = useState(false);

    // 检测屏幕宽度
    useEffect(() => {
        const check = () => {
            setIsNarrow(window.innerWidth < window.innerHeight);
        };
        check();
        window.addEventListener("resize", check);
        return () => window.removeEventListener("resize", check);
    }, []);

    // 控制状态
    const throttleRef = useRef(0);
    const directionRef = useRef(0);

    // 电机轮询
    const motorIntervalRef = useRef<number | null>(null);
    const stopControlRef = useRef<(() => void) | null>(null);
    const lastCommandRef = useRef<{left: number, right: number} | null>(null);

    const sendCommand = useCallback(async () => {
        const thr = throttleRef.current;
        const dir = directionRef.current;

        // 死区：摇杆接近归零时视为0
        const deadZone = 3;
        const thrZero = Math.abs(thr) < deadZone ? 0 : thr;
        const dirZero = Math.abs(dir) < deadZone ? 0 : dir;


        const base = thrZero;
        const turn = dirZero * 0.5;
        const left = Math.max(-100, Math.min(100, base - turn));
        const right = Math.max(-100, Math.min(100, base + turn));

        // 只有值变化超过10才发送
        if (lastCommandRef.current &&
            Math.abs(lastCommandRef.current.left - left) < 10 &&
            Math.abs(lastCommandRef.current.right - right) < 10) {
            return;  // 变化太小，不发送
        }
        lastCommandRef.current = {left, right};

        // motor_direct 返回时已带速度信息
        try {
            const res = await fetch(`/api/motor_direct?left=${left}&right=${right}&duration=0`);
            if (res.ok) {
                const data = await res.json();
                setLeftSpeed(data.left_speed ?? 0);
                setRightSpeed(data.right_speed ?? 0);
            }
        } catch {
            // 忽略
        }
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
            fetch("/api/motor_direct?left=0&right=0&duration=0").catch(() => {});
            setLeftSpeed(0);
            setRightSpeed(0);
        }
    }, []);

    // 保持 ref 指向最新的 stopControl
    stopControlRef.current = stopControl;

    // 油门摇杆（窄屏时水平放置）
    const throttleRefEl = useRef<HTMLDivElement>(null);
    const throttleActiveRef = useRef(false);

    const handleThrottleMove = useCallback((clientX: number, clientY: number) => {
        if (!throttleRefEl.current) return;
        const rect = throttleRefEl.current.getBoundingClientRect();
        let dist, maxDist;

        if (isNarrow) {
            // 窄屏：水平滑动
            const centerX = rect.left + rect.width / 2;
            const dx = clientX - centerX;
            maxDist = rect.width / 2 - 20;
            dist = Math.max(-maxDist, Math.min(maxDist, dx));
            const thr = Math.round((dist / maxDist) * 100);
            throttleRef.current = thr;
            setThrottleDisplay(thr);
        } else {
            // 宽屏：垂直滑动
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
        } else if (throttleRef.current === 0 && throttleActiveRef.current) {
            throttleActiveRef.current = false;
            stopControlRef.current?.();
        }
    }, [isNarrow, startControl]);

    const handleThrottleEnd = useCallback(() => {
        throttleRef.current = 0;
        setThrottleDisplay(0);
        throttleActiveRef.current = false;
        // 油门松手时也重置方向，确保完全停车
        directionRef.current = 0;
        setDirectionDisplay(0);
        stopControlRef.current?.();
    }, []);

    // 方向摇杆（窄屏时垂直放置）
    const directionRefEl = useRef<HTMLDivElement>(null);
    const directionActiveRef = useRef(false);

    const handleDirectionMove = useCallback((clientX: number, clientY: number) => {
        if (!directionRefEl.current) return;
        const rect = directionRefEl.current.getBoundingClientRect();
        let dist, maxDist;

        if (isNarrow) {
            // 窄屏：垂直滑动
            const centerY = rect.top + rect.height / 2;
            const dy = clientY - centerY;
            maxDist = rect.height / 2 - 20;
            dist = Math.max(-maxDist, Math.min(maxDist, dy));
            const dir = Math.round((-dist / maxDist) * 100);
            directionRef.current = dir;
            setDirectionDisplay(dir);
        } else {
            // 宽屏：水平滑动
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
        } else if (directionRef.current === 0 && directionActiveRef.current) {
            directionActiveRef.current = false;
            stopControlRef.current?.();
        }
    }, [isNarrow, startControl]);

    const handleDirectionEnd = useCallback(() => {
        directionRef.current = 0;
        setDirectionDisplay(0);
        directionActiveRef.current = false;
        stopControlRef.current?.();
    }, []);

    // 返回
    const handleBack = () => {
        stopControl();
        navigator.sendBeacon("/api/video_stream/close");
        window.location.href = "/";
    };

    // 尺寸
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
                paddingBottom: "calc(env(safe-area-inset-bottom) + 10px)",
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
            {/* 油门摇杆 */}
            <div style={{display: "flex", flexDirection: "column", alignItems: "center", flex: 1}}>
                <div style={{fontSize: "12px", opacity: 0.6, marginBottom: "8px", transform: isNarrow ? "rotate(90deg)" : "none"}}>油门</div>
                <div
                    ref={throttleRefEl}
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
                    {/* 中线 */}
                    <div style={{
                        position: "absolute",
                        left: "50%",
                        top: "50%",
                        transform: "translate(-50%, -50%)",
                        width: isNarrow ? "2px" : "30px",
                        height: isNarrow ? "30px" : "2px",
                        background: "#475569",
                    }}/>
                    {/* 滑块 */}
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
            }}>
                {/* 视频流 */}
                <div
                    style={{
                        borderRadius: "12px",
                        overflow: "hidden",
                        border: "2px solid #334155",
                    }}
                >
                    <img
                        src="/api/video_stream"
                        alt="Camera"
                        style={{
                            width: `${videoW}px`,
                            height: `${videoH}px`,
                            objectFit: "cover",
                            background: "#000",
                        }}
                    />
                </div>

                {/* 速度显示 */}
                <div style={{display: "flex", justifyContent: "center", gap: "20px", fontSize: "13px", marginTop: "10px"}}>
                    <div>左 <span style={{color: "#4ade80", fontWeight: "bold"}}>{leftSpeed > 0 ? "+" : ""}{leftSpeed.toFixed(2)}</span></div>
                    <div>右 <span style={{color: "#4ade80", fontWeight: "bold"}}>{rightSpeed > 0 ? "+" : ""}{rightSpeed.toFixed(2)}</span></div>
                </div>

                {/* 返回按钮 */}
                <button
                    onClick={handleBack}
                    style={{
                        marginTop: "10px",
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
                    {/* 中线 */}
                    <div style={{
                        position: "absolute",
                        left: "50%",
                        top: "50%",
                        transform: "translate(-50%, -50%)",
                        width: isNarrow ? "30px" : "2px",
                        height: isNarrow ? "2px" : "30px",
                        background: "#475569",
                    }}/>
                    {/* 滑块 */}
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
