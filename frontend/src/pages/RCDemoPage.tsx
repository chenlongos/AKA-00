import {useEffect, useRef, useState, useCallback} from "react";


const RCDemoPage = () => {
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [connected, setConnected] = useState(false);
    const [direction, setDirection] = useState(0); // -100 ~ 100 (左 ~ 右)
    const [throttle, setThrottle] = useState(0);   // -100 ~ 100 (倒 ~ 前)
    const [braking, setBraking] = useState(false);

    // 方向和油门值
    const directionRef = useRef(0);
    const throttleRef = useRef(0);

    // 检查连接状态
    useEffect(() => {
        const checkConnection = async () => {
            try {
                const res = await fetch("/api/heartbeat");
                if (res.ok) setConnected(true);
                else setConnected(false);
            } catch {
                setConnected(false);
            }
        };
        checkConnection();
        const t = setInterval(checkConnection, 5000);
        return () => clearInterval(t);
    }, []);

    // 电机控制 + 状态轮询（事件驱动）
    const motorIntervalRef = useRef<number | null>(null);

    const updateAndFetch = async () => {
        const dir = directionRef.current;
        const thr = throttleRef.current;
        let left, right;

        if (braking) {
            left = 0;
            right = 0;
        } else if (thr === 0) {
            left = 0;
            right = 0;
        } else {
            const base = thr;
            const turn = dir * 0.5;
            left = Math.max(-100, Math.min(100, base - turn));
            right = Math.max(-100, Math.min(100, base + turn));
        }

        fetch(`/api/motor_direct?left=${left}&right=${right}&duration=0`).catch(() => {});

        try {
            const timestamp = Date.now();
            const res = await fetch(`/api/motor_status?timestamp=${timestamp}`);
            if (res.ok) {
                const data = await res.json();
                setLeftSpeed(data.left_speed ?? 0);
                setRightSpeed(data.right_speed ?? 0);
            }
        } catch {
            // 忽略错误
        }
    };

    const startControl = () => {
        if (motorIntervalRef.current !== null) return;
        updateAndFetch();
        motorIntervalRef.current = window.setInterval(updateAndFetch, 100);
    };

    const stopControl = () => {
        if (motorIntervalRef.current !== null) {
            clearInterval(motorIntervalRef.current);
            motorIntervalRef.current = null;
            fetch("/api/motor_direct?left=0&right=0&duration=0").catch(() => {});
        }
    };

    // 页面离开时停止电机并关闭摄像头
    useEffect(() => {
        return () => {
            fetch("/api/motor_direct?left=0&right=0&duration=0").catch(() => {});
            navigator.sendBeacon("/api/video_stream/close");
        };
    }, []);

    // 方向摇杆控制
    const directionRefEl = useRef<HTMLDivElement>(null);

    const handleDirectionMove = useCallback((clientX: number) => {
        if (!directionRefEl.current) return;
        const rect = directionRefEl.current.getBoundingClientRect();
        const centerX = rect.left + rect.width / 2;
        const dx = clientX - centerX;
        const maxDist = rect.width / 2 - 20;
        const dist = Math.max(-maxDist, Math.min(maxDist, dx));
        const dir = Math.round((dist / maxDist) * 100);
        directionRef.current = dir;
        setDirection(dir);
    }, []);

    const handleDirectionEnd = useCallback(() => {
        directionRef.current = 0;
        setDirection(0);
        stopControl();
    }, []);

    // 油门控制
    const throttleRefEl = useRef<HTMLDivElement>(null);

    const handleThrottleMove = useCallback((clientY: number) => {
        if (!throttleRefEl.current) return;
        const rect = throttleRefEl.current.getBoundingClientRect();
        const centerY = rect.top + rect.height / 2;
        const dy = clientY - centerY;
        const maxDist = rect.height / 2 - 20;
        const dist = Math.max(-maxDist, Math.min(maxDist, dy));
        const thr = Math.round((-dist / maxDist) * 100); // 上为正
        throttleRef.current = thr;
        setThrottle(thr);
        setBraking(false);
    }, []);

    const handleThrottleEnd = useCallback(() => {
        throttleRef.current = 0;
        setThrottle(0);
        stopControl();
    }, []);

    // 返回按钮
    const handleBack = () => {
        fetch("/api/motor_direct?left=0&right=0&duration=0").catch(() => {});
        navigator.sendBeacon("/api/video_stream/close");
        window.location.href = "/";
    };

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
                flexDirection: "column",
                alignItems: "center",
                overflow: "hidden",
            }}
        >
            {/* 顶部 */}
            <div style={{display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "8px", width: "100%", maxWidth: "400px"}}>
                <h2 style={{margin: 0, fontSize: "18px"}}>赛车模式</h2>
                <div style={{display: "flex", alignItems: "center", gap: "15px"}}>
                    <span style={{fontSize: "13px", opacity: 0.7}}>
                        <span style={{color: connected ? "#4ade80" : "#ef4444"}}>●</span> {connected ? "已连接" : "未连接"}
                    </span>
                    <button
                        onClick={handleBack}
                        style={{
                            background: "#334155",
                            border: "none",
                            color: "white",
                            padding: "6px 12px",
                            borderRadius: "6px",
                            fontSize: "12px",
                            cursor: "pointer",
                        }}
                    >
                        返回
                    </button>
                </div>
            </div>

            {/* 视频流 */}
            <div
                style={{
                    borderRadius: "12px",
                    overflow: "hidden",
                    border: "2px solid #334155",
                    display: "inline-block",
                    marginBottom: "10px",
                }}
            >
                <img
                    src="/api/video_stream"
                    alt="Camera"
                    style={{
                        width: "280px",
                        height: "210px",
                        objectFit: "cover",
                        background: "#000",
                    }}
                />
            </div>

            {/* 速度显示 */}
            <div style={{display: "flex", justifyContent: "center", gap: "30px", fontSize: "13px", marginBottom: "10px"}}>
                <div>左 <span style={{color: "#4ade80", fontWeight: "bold"}}>{leftSpeed > 0 ? "+" : ""}{leftSpeed}</span></div>
                <div>右 <span style={{color: "#4ade80", fontWeight: "bold"}}>{rightSpeed > 0 ? "+" : ""}{rightSpeed}</span></div>
            </div>

            {/* 方向盘 + 油门布局 */}
            <div style={{display: "flex", justifyContent: "center", gap: "25px", alignItems: "flex-start"}}>

                {/* 方向摇杆 */}
                <div style={{display: "flex", flexDirection: "column", alignItems: "center"}}>
                    <div style={{fontSize: "12px", opacity: 0.6, marginBottom: "8px"}}>方向</div>
                    <div
                        ref={directionRefEl}
                        onPointerDown={startControl}
                        onPointerMove={(e) => handleDirectionMove(e.clientX)}
                        onPointerUp={handleDirectionEnd}
                        onPointerLeave={handleDirectionEnd}
                        style={{
                            width: "160px",
                            height: "45px",
                            borderRadius: "22px",
                            background: "rgba(255,255,255,0.1)",
                            border: "2px solid #334155",
                            position: "relative",
                            touchAction: "none",
                        }}
                    >
                        {/* 中心标记 */}
                        <div style={{
                            position: "absolute",
                            left: "50%",
                            top: "50%",
                            transform: "translate(-50%, -50%)",
                            width: "4px",
                            height: "30px",
                            background: "#475569",
                            borderRadius: "2px",
                        }}/>
                        {/* 滑块 */}
                        <div style={{
                            position: "absolute",
                            width: "35px",
                            height: "35px",
                            borderRadius: "50%",
                            background: direction === 0 ? "#475569" : "#2563eb",
                            border: "3px solid #64748b",
                            top: "50%",
                            left: "50%",
                            transform: `translate(calc(-50% + ${direction * 0.6}px), -50%)`,
                            transition: direction === 0 ? "background 0.1s" : "none",
                            boxShadow: "0 2px 8px rgba(0,0,0,0.3)",
                        }}/>
                    </div>
                    <div style={{fontSize: "12px", marginTop: "4px", opacity: 0.6}}>{direction > 0 ? "→" : direction < 0 ? "←" : "·"} {Math.abs(direction)}%</div>
                </div>

                {/* 油门/刹车 */}
                <div style={{display: "flex", flexDirection: "column", alignItems: "center"}}>
                    <div style={{fontSize: "12px", opacity: 0.6, marginBottom: "8px"}}>油门</div>
                    <div
                        ref={throttleRefEl}
                        onPointerDown={startControl}
                        onPointerMove={(e) => handleThrottleMove(e.clientY)}
                        onPointerUp={handleThrottleEnd}
                        onPointerLeave={handleThrottleEnd}
                        style={{
                            width: "55px",
                            height: "120px",
                            borderRadius: "27px",
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
                            width: "35px",
                            height: "2px",
                            background: "#475569",
                        }}/>
                        {/* 滑块 */}
                        <div style={{
                            position: "absolute",
                            width: "45px",
                            height: "45px",
                            borderRadius: "50%",
                            background: throttle >= 0 ? "#22c55e" : "#ef4444",
                            border: "3px solid #64748b",
                            left: "50%",
                            top: "50%",
                            transform: `translate(-50%, calc(-50% - ${throttle * 0.35}px))`,
                            transition: "background 0.1s",
                            boxShadow: "0 2px 8px rgba(0,0,0,0.3)",
                        }}/>
                    </div>
                    <div style={{fontSize: "12px", marginTop: "4px", opacity: 0.6}}>
                        {throttle > 0 ? "▲" : throttle < 0 ? "▼" : "·"} {Math.abs(throttle)}%
                    </div>
                </div>

                {/* 刹车按钮 */}
                <div style={{display: "flex", flexDirection: "column", alignItems: "center"}}>
                    <div style={{fontSize: "12px", opacity: 0.6, marginBottom: "8px"}}>刹车</div>
                    <button
                        onPointerDown={() => { setBraking(true); startControl(); }}
                        onPointerUp={() => { setBraking(false); stopControl(); }}
                        onPointerLeave={() => { setBraking(false); stopControl(); }}
                        style={{
                            width: "55px",
                            height: "120px",
                            borderRadius: "27px",
                            background: braking ? "#ef4444" : "rgba(255,255,255,0.1)",
                            border: "2px solid #334155",
                            color: "white",
                            fontSize: "14px",
                            fontWeight: "bold",
                            cursor: "pointer",
                            touchAction: "none",
                            transition: "background 0.1s",
                        }}
                    >
                        STOP
                    </button>
                </div>
            </div>

            {/* 状态 */}
            <div style={{marginTop: "10px", fontSize: "14px", opacity: 0.8}}>
                方向: {direction}% | 油门: {throttle}% | {braking ? "刹车中" : "行驶中"}
            </div>
        </div>
    );
};

export default RCDemoPage;
