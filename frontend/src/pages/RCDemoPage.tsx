import {useEffect, useRef, useState, useCallback} from "react";


const RCDemoPage = () => {
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [status, setStatus] = useState("准备就绪");
    const [connected, setConnected] = useState(false);
    const activeKeys = useRef<Set<string>>(new Set());
    const actionIntervalRef = useRef<number | null>(null);

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

    // 定时获取电机实时速度
    useEffect(() => {
        const fetchSpeed = async () => {
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
        fetchSpeed();
        const interval = setInterval(fetchSpeed, 200);
        return () => clearInterval(interval);
    }, []);

    // 页面离开时关闭摄像头
    useEffect(() => {
        return () => {
            navigator.sendBeacon("/api/video_stream/close", "1");
        };
    }, []);

    // 发送控制命令
    const sendAction = useCallback(async (action: string) => {
        try {
            await fetch(`/api/control?action=${action}&speed=50&time=0`);
        } catch {
            // 忽略错误
        }
    }, []);

    // 根据当前按键计算动作
    const updateAction = useCallback(() => {
        const keys = activeKeys.current;
        let action = "stop";

        if (keys.has("w") || keys.has("ArrowUp") || keys.has("W")) {
            action = "up";
        } else if (keys.has("s") || keys.has("ArrowDown") || keys.has("S")) {
            action = "down";
        } else if (keys.has("a") || keys.has("ArrowLeft") || keys.has("A")) {
            action = "left";
        } else if (keys.has("d") || keys.has("ArrowRight") || keys.has("D")) {
            action = "right";
        }

        setStatus(action === "stop" ? "停止" : `移动中: ${action}`);
        sendAction(action);
    }, [sendAction]);

    // 持续更新动作
    useEffect(() => {
        actionIntervalRef.current = window.setInterval(updateAction, 50);
        return () => {
            if (actionIntervalRef.current) {
                clearInterval(actionIntervalRef.current);
            }
        };
    }, [updateAction]);

    // 键盘事件处理
    useEffect(() => {
        const handleKeyDown = (e: KeyboardEvent) => {
            // 防止方向键滚动页面
            if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", " "].includes(e.key)) {
                e.preventDefault();
            }
            activeKeys.current.add(e.key);
            updateAction();
        };

        const handleKeyUp = (e: KeyboardEvent) => {
            activeKeys.current.delete(e.key);
            updateAction();
        };

        window.addEventListener("keydown", handleKeyDown);
        window.addEventListener("keyup", handleKeyUp);
        return () => {
            window.removeEventListener("keydown", handleKeyDown);
            window.removeEventListener("keyup", handleKeyUp);
        };
    }, [updateAction]);

    // 触摸/鼠标虚拟摇杆
    const joystickRef = useRef<HTMLDivElement>(null);
    const [joystickDir, setJoystickDir] = useState<{x: number; y: number} | null>(null);

    const handleJoystick = useCallback((clientX: number, clientY: number) => {
        if (!joystickRef.current) return;
        const rect = joystickRef.current.getBoundingClientRect();
        const centerX = rect.left + rect.width / 2;
        const centerY = rect.top + rect.height / 2;
        const dx = clientX - centerX;
        const dy = clientY - centerY;
        const maxDist = rect.width / 2;
        const dist = Math.min(Math.sqrt(dx * dx + dy * dy), maxDist);

        setJoystickDir({x: dx, y: dy});

        // 根据方向设置按键
        activeKeys.current.clear();
        if (dist > 10) {
            if (Math.abs(dx) > Math.abs(dy) * 1.5) {
                // 水平方向
                if (dx > 0) activeKeys.current.add("d");
                else activeKeys.current.add("a");
            } else if (Math.abs(dy) > Math.abs(dx) * 1.5) {
                // 垂直方向
                if (dy < 0) activeKeys.current.add("w");
                else activeKeys.current.add("s");
            }
        }
        updateAction();
    }, [updateAction]);

    const handleJoystickEnd = useCallback(() => {
        setJoystickDir(null);
        activeKeys.current.clear();
        updateAction();
    }, [updateAction]);

    return (
        <div
            style={{
                fontFamily: "system-ui, sans-serif",
                background: "#0f172a",
                color: "white",
                minHeight: "100vh",
                padding: "10px",
                textAlign: "center",
                userSelect: "none",
            }}
        >
            {/* 顶部状态栏 */}
            <div style={{display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: "10px"}}>
                <h2 style={{margin: 0, fontSize: "18px"}}>遥控模式</h2>
                <div style={{display: "flex", alignItems: "center", gap: "15px"}}>
                    <span style={{fontSize: "13px", opacity: 0.7}}>
                        <span style={{color: connected ? "#4ade80" : "#ef4444"}}>●</span> {connected ? "已连接" : "未连接"}
                    </span>
                    <button
                        onClick={() => { fetch("/api/video_stream/close", {method: "POST"}).catch(() => {}); window.location.href = "/"; }}
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
                        width: "320px",
                        height: "240px",
                        objectFit: "cover",
                        display: "block",
                        background: "#000",
                    }}
                />
            </div>

            {/* 电机速度 */}
            <div
                style={{
                    display: "flex",
                    justifyContent: "center",
                    gap: "30px",
                    fontSize: "13px",
                    marginBottom: "15px",
                }}
            >
                <div>左 <span style={{color: "#4ade80", fontWeight: "bold"}}>{leftSpeed > 0 ? "+" : ""}{leftSpeed}</span></div>
                <div>右 <span style={{color: "#4ade80", fontWeight: "bold"}}>{rightSpeed > 0 ? "+" : ""}{rightSpeed}</span></div>
            </div>

            {/* 操作提示 */}
            <div style={{fontSize: "12px", opacity: 0.6, marginBottom: "10px"}}>
                WASD / 方向键 / 触摸摇杆 控制
            </div>

            {/* 虚拟摇杆区域 */}
            <div
                ref={joystickRef}
                onPointerMove={(e) => handleJoystick(e.clientX, e.clientY)}
                onPointerUp={handleJoystickEnd}
                onPointerLeave={handleJoystickEnd}
                style={{
                    width: "150px",
                    height: "150px",
                    borderRadius: "50%",
                    background: "rgba(255,255,255,0.1)",
                    border: "2px solid #334155",
                    margin: "0 auto 15px",
                    position: "relative",
                    touchAction: "none",
                }}
            >
                {/* 摇杆中心点 */}
                <div
                    style={{
                        position: "absolute",
                        width: "50px",
                        height: "50px",
                        borderRadius: "50%",
                        background: joystickDir ? "#2563eb" : "#475569",
                        border: "3px solid #64748b",
                        left: "50%",
                        top: "50%",
                        transform: joystickDir
                            ? `translate(calc(-50% + ${Math.min(joystickDir.x, 40)}px), calc(-50% + ${Math.min(joystickDir.y, 40)}px))`
                            : "translate(-50%, -50%)",
                        transition: joystickDir ? "none" : "transform 0.1s",
                        boxShadow: "0 2px 8px rgba(0,0,0,0.3)",
                    }}
                />
            </div>

            {/* 状态 */}
            <div style={{fontSize: "14px", opacity: 0.8}}>
                {status}
            </div>
        </div>
    );
};

export default RCDemoPage;
