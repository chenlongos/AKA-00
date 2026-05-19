import {useState, useEffect, useRef, useCallback} from "react";
import {api} from "../api";

const DEADZONE = 18;
const SPEED_LR = 25;

const cmdLabel: Record<string, string> = {
    up: "前进", down: "后退", left: "左转", right: "右转", stop: "停止",
};

const GravityControlPage = () => {
    const [pitch, setPitch] = useState(0);
    const [roll, setRoll] = useState(0);
    const [cmd, setCmd] = useState("stop");
    const [active, setActive] = useState(false);
    const [permissionNeeded, setPermissionNeeded] = useState(false);
    const [speedFB, setSpeedFB] = useState(50);
    const cmdRef = useRef("stop");
    const speedFBRef = useRef(50);
    const dpadActiveRef = useRef(false);
    const wakeLockRef = useRef<any>(null);
    const isSecure = typeof window !== "undefined" && window.isSecureContext;

    useEffect(() => { speedFBRef.current = speedFB; }, [speedFB]);

    const vibrate = useCallback(() => {
        try { window.navigator?.vibrate?.(15); } catch {}
    }, []);

    const sendCmd = useCallback((nextCmd: string) => {
        if (nextCmd === cmdRef.current) return;
        cmdRef.current = nextCmd;
        setCmd(nextCmd);
        vibrate();

        if (nextCmd === "stop") {
            api.motor.action("stop").catch(() => {});
        } else if (nextCmd === "grab" || nextCmd === "release") {
            api.motor.action(nextCmd).catch(() => {});
        } else {
            const speed = (nextCmd === "left" || nextCmd === "right") ? SPEED_LR : speedFBRef.current;
            api.motor.action(nextCmd, speed).catch(() => {});
        }
    }, [vibrate]);

    // 重力感应
    useEffect(() => {
        if (!active) return;
        const handleOrientation = (e: DeviceOrientationEvent) => {
            if (dpadActiveRef.current) return;
            const b = e.beta ?? 0;
            const g = e.gamma ?? 0;
            setPitch(Math.round(b));
            setRoll(Math.round(g));

            let nextCmd: string;
            if (b > DEADZONE) nextCmd = "down";
            else if (b < -DEADZONE) nextCmd = "up";
            else if (g > DEADZONE) nextCmd = "right";
            else if (g < -DEADZONE) nextCmd = "left";
            else nextCmd = "stop";
            sendCmd(nextCmd);
        };
        window.addEventListener("deviceorientation", handleOrientation);
        return () => window.removeEventListener("deviceorientation", handleOrientation);
    }, [active, sendCmd]);

    // 屏幕常亮
    useEffect(() => {
        if (active) {
            (async () => {
                try {
                    // @ts-ignore
                    if ("wakeLock" in navigator && navigator.wakeLock)
                        // @ts-ignore
                        wakeLockRef.current = await navigator.wakeLock.request("screen");
                } catch {}
            })();
        } else {
            if (wakeLockRef.current) { try { wakeLockRef.current.release(); } catch {}; wakeLockRef.current = null; }
            sendCmd("stop");
        }
        return () => { if (wakeLockRef.current) { try { wakeLockRef.current.release(); } catch {} } };
    }, [active, sendCmd]);

    const startGravity = async () => {
        if (!isSecure) return;
        if (typeof (DeviceOrientationEvent as any).requestPermission === "function") {
            try {
                const perm = await (DeviceOrientationEvent as any).requestPermission();
                if (perm !== "granted") return;
            } catch { setPermissionNeeded(true); return; }
        }
        setActive(true);
    };

    const stopGravity = () => setActive(false);

    // 十字键
    const handleDPadDown = useCallback((nextCmd: string) => {
        dpadActiveRef.current = true;
        sendCmd(nextCmd);
    }, [sendCmd]);

    const handleDPadUp = useCallback(() => {
        dpadActiveRef.current = false;
        sendCmd("stop");
    }, [sendCmd]);

    const arrowIcon = cmd === "up" ? "▲" : cmd === "down" ? "▼" : cmd === "left" ? "◀" : cmd === "right" ? "▶" : "●";
    const isMoving = cmd !== "stop";

    const dpadBtn = (dir: string, icon: string) => (
        <button
            onPointerDown={(e) => { e.preventDefault(); handleDPadDown(dir); }}
            onPointerUp={(e) => { e.preventDefault(); handleDPadUp(); }}
            onPointerLeave={handleDPadUp}
            style={{
                width: "100%", height: "100%", borderRadius: "12px", border: "1px solid",
                display: "flex", alignItems: "center", justifyContent: "center",
                fontSize: "20px",
                background: cmd === dir ? "#2563eb" : "#1e293b",
                borderColor: cmd === dir ? "#60a5fa" : "#334155",
                color: cmd === dir ? "#fff" : "#64748b",
                boxShadow: cmd === dir ? "0 0 15px rgba(37,99,235,0.4)" : "none",
                transition: "all 0.15s ease",
                touchAction: "none",
                WebkitTapHighlightColor: "transparent",
                cursor: "pointer",
                outline: "none",
            }}
        >
            {icon}
        </button>
    );

    return (
        <div style={{
            position: "fixed", inset: 0, background: "#020617", color: "#f1f5f9",
            display: "flex", flexDirection: "column", alignItems: "center",
            userSelect: "none", overflow: "hidden", touchAction: "none",
            padding: "16px", paddingBottom: "48px",
        }}>
            {/* 非安全上下文遮罩 */}
            {!isSecure && (
                <div style={{
                    position: "fixed", inset: 0, background: "rgba(2,6,23,0.95)", zIndex: 60,
                    display: "flex", alignItems: "center", justifyContent: "center",
                    padding: "32px", textAlign: "center",
                }}>
                    <div style={{
                        background: "#0f172a", border: "1px solid #334155",
                        padding: "32px", borderRadius: "24px",
                        display: "flex", flexDirection: "column", gap: "16px",
                    }}>
                        <div style={{fontSize: "48px"}}>&#128683;</div>
                        <h2 style={{fontSize: "20px", fontWeight: 700}}>环境限制</h2>
                        <p style={{fontSize: "14px", color: "#94a3b8", lineHeight: 1.6}}>
                            Chrome 需要 <b>HTTPS</b> 协议或 <b>Localhost</b> 才能开启重力感应。手动操作按键仍可正常使用。
                        </p>
                    </div>
                </div>
            )}

            {/* 顶部信息栏 */}
            <div style={{
                width: "100%", maxWidth: "512px",
                display: "flex", flexDirection: "column", gap: "8px",
                marginBottom: "16px",
            }}>
                <div style={{display: "flex", justifyContent: "space-between", alignItems: "flex-end"}}>
                    <div style={{display: "flex", flexDirection: "column"}}>
                        <span style={{
                            color: "#3b82f6", fontWeight: 900, fontStyle: "italic",
                            letterSpacing: "0.1em", fontSize: "18px",
                        }}>
                            AI 智能控制
                        </span>
                        <span style={{fontSize: "10px", color: "#64748b", fontFamily: "monospace"}}>
                            {!isSecure ? "⚠️ 非 HTTPS 环境 (陀螺仪将受限)" : `状态: ${active ? "重力遥控已激活" : "已就绪"}`}
                        </span>
                    </div>
                    <div style={{
                        background: "#0f172a", border: "1px solid #334155",
                        borderRadius: "8px", display: "flex", alignItems: "center",
                        padding: "4px 8px",
                    }}>
                        <span style={{fontSize: "10px", color: "#94a3b8", marginRight: "8px", textTransform: "uppercase"}}>速度</span>
                        <span style={{color: "#60a5fa", fontWeight: 700, fontFamily: "monospace"}}>{speedFB}</span>
                    </div>
                </div>
            </div>

            {/* HUD 仪表盘 */}
            <div style={{
                flex: 1, width: "100%", position: "relative",
                display: "flex", alignItems: "center", justifyContent: "center",
            }}>
                {/* 外圈 */}
                <div style={{
                    position: "absolute", width: "256px", height: "256px",
                    border: "1px solid rgba(59,130,246,0.1)", borderRadius: "50%",
                }} />
                {/* 内圈 — 可旋转核心 */}
                <div style={{
                    position: "relative", width: "160px", height: "160px",
                    border: "2px solid rgba(59,130,246,0.4)", borderRadius: "50%",
                    display: "flex", alignItems: "center", justifyContent: "center",
                    transform: `rotateX(${pitch}deg) rotateY(${roll}deg)`,
                    transition: "transform 0.1s ease-out",
                    boxShadow: "0 0 30px rgba(59,130,246,0.1)",
                }}>
                    <div style={{
                        position: "absolute", width: "100%", height: "1px",
                        background: "rgba(59,130,246,0.3)",
                    }} />
                    <div style={{
                        position: "absolute", width: "1px", height: "100%",
                        background: "rgba(59,130,246,0.3)",
                    }} />
                    <div style={{
                        position: "absolute", width: "48px", height: "48px",
                        display: "flex", alignItems: "center", justifyContent: "center",
                        fontSize: "24px",
                        color: isMoving ? "#f97316" : "rgba(59,130,246,0.5)",
                        transform: isMoving ? "scale(1.1)" : "scale(1)",
                        transition: "all 0.3s",
                    }}>
                        {arrowIcon}
                    </div>
                </div>
                {/* 命令标签 */}
                <div style={{
                    position: "absolute", bottom: "16px",
                    padding: "4px 20px", background: "#2563eb",
                    borderRadius: "9999px", fontSize: "12px", fontWeight: 900,
                    textTransform: "uppercase", letterSpacing: "0.1em",
                    boxShadow: "0 10px 15px -3px rgba(0,0,0,0.1), 0 4px 6px -4px rgba(0,0,0,0.1)",
                }}>
                    {cmdLabel[cmd] || "待机"}
                </div>
            </div>

            {/* 控制面板 — 2列布局 */}
            <div style={{
                width: "100%", maxWidth: "512px",
                display: "grid", gridTemplateColumns: "1fr 1fr", gap: "16px",
            }}>
                {/* 左列：速度 + 重力开关 + 抓取释放 */}
                <div style={{display: "flex", flexDirection: "column", gap: "16px"}}>
                    {/* 速度调节 */}
                    <div style={{
                        background: "rgba(15,23,42,0.8)", padding: "12px",
                        borderRadius: "16px", border: "1px solid #1e293b",
                    }}>
                        <span style={{
                            fontSize: "10px", color: "#64748b", display: "block",
                            marginBottom: "8px", fontWeight: 700, textTransform: "uppercase",
                        }}>
                            速度设定
                        </span>
                        <input
                            type="range" min="30" max="70" value={speedFB}
                            onChange={(e) => setSpeedFB(parseInt(e.target.value))}
                            style={{
                                width: "100%", height: "24px",
                                WebkitAppearance: "none",
                                background: "#1e293b", borderRadius: "10px",
                                margin: 0,
                            }}
                        />
                    </div>

                    {/* 重力开关 */}
                    <button
                        onClick={active ? stopGravity : startGravity}
                        style={{
                            display: "flex", flexDirection: "column", alignItems: "center",
                            justifyContent: "center", padding: "16px",
                            borderRadius: "16px", border: "1px solid",
                            background: active ? "#ea580c" : "#0f172a",
                            borderColor: active ? "#fb923c" : "#1e293b",
                            color: active ? "#fff" : "#94a3b8",
                            boxShadow: active ? "0 10px 15px -3px rgba(0,0,0,0.1)" : "none",
                            transition: "all 0.15s ease",
                            cursor: "pointer",
                            touchAction: "manipulation",
                        }}
                    >
                        <span style={{fontSize: "20px", marginBottom: "4px"}}>
                            {active ? "📡 开启中" : "📴 重力感应"}
                        </span>
                        <span style={{fontSize: "10px", fontWeight: 700, textTransform: "uppercase"}}>
                            点此切换模式
                        </span>
                    </button>

                    {/* 抓取 / 释放 */}
                    <div style={{display: "grid", gridTemplateColumns: "1fr 1fr", gap: "8px"}}>
                        <button
                            onPointerDown={(e) => { e.preventDefault(); sendCmd("grab"); }}
                            onPointerUp={(e) => { e.preventDefault(); sendCmd("stop"); }}
                            onPointerLeave={() => sendCmd("stop")}
                            style={{
                                background: "#0f172a", border: "1px solid #1e293b",
                                padding: "12px", borderRadius: "12px",
                                display: "flex", flexDirection: "column",
                                alignItems: "center", justifyContent: "center",
                                color: "#94a3b8", cursor: "pointer",
                                touchAction: "none",
                                WebkitTapHighlightColor: "transparent",
                            }}
                        >
                            <span style={{fontSize: "18px"}}>🤏</span>
                            <span style={{fontSize: "8px", fontWeight: 700, textTransform: "uppercase", color: "#64748b", marginTop: "2px"}}>抓取</span>
                        </button>
                        <button
                            onPointerDown={(e) => { e.preventDefault(); sendCmd("release"); }}
                            onPointerUp={(e) => { e.preventDefault(); sendCmd("stop"); }}
                            onPointerLeave={() => sendCmd("stop")}
                            style={{
                                background: "#0f172a", border: "1px solid #1e293b",
                                padding: "12px", borderRadius: "12px",
                                display: "flex", flexDirection: "column",
                                alignItems: "center", justifyContent: "center",
                                color: "#94a3b8", cursor: "pointer",
                                touchAction: "none",
                                WebkitTapHighlightColor: "transparent",
                            }}
                        >
                            <span style={{fontSize: "18px"}}>👐</span>
                            <span style={{fontSize: "8px", fontWeight: 700, textTransform: "uppercase", color: "#64748b", marginTop: "2px"}}>释放</span>
                        </button>
                    </div>
                </div>

                {/* 右列：十字键 */}
                <div style={{
                    aspectRatio: "1",
                    background: "rgba(15,23,42,0.5)", borderRadius: "24px",
                    padding: "12px", border: "1px solid #1e293b",
                    display: "grid", gridTemplateColumns: "1fr 1fr 1fr",
                    gridTemplateRows: "1fr 1fr 1fr", gap: "8px",
                }}>
                    <div style={{gridColumn: "2", gridRow: "1"}}>
                        {dpadBtn("up", "▲")}
                    </div>
                    <div style={{gridColumn: "1", gridRow: "2"}}>
                        {dpadBtn("left", "◀")}
                    </div>
                    <div style={{gridColumn: "2", gridRow: "2"}}>
                        <button
                            onPointerDown={(e) => { e.preventDefault(); handleDPadDown("stop"); }}
                            style={{
                                width: "100%", height: "100%", borderRadius: "12px",
                                border: "1px solid rgba(239,68,68,0.3)",
                                background: "rgba(239,68,68,0.1)", color: "#ef4444",
                                fontSize: "10px", fontWeight: 900,
                                touchAction: "none",
                                WebkitTapHighlightColor: "transparent",
                                cursor: "pointer",
                                outline: "none",
                            }}
                        >
                            停止
                        </button>
                    </div>
                    <div style={{gridColumn: "3", gridRow: "2"}}>
                        {dpadBtn("right", "▶")}
                    </div>
                    <div style={{gridColumn: "2", gridRow: "3"}}>
                        {dpadBtn("down", "▼")}
                    </div>
                </div>
            </div>

            {/* 返回按钮 */}
            <div style={{marginTop: "16px"}}>
                <button
                    onClick={() => { stopGravity(); window.location.href = "/"; }}
                    style={{
                        background: "rgba(255,255,255,0.1)", border: "1px solid #334155",
                        color: "#fff", padding: "8px 24px", borderRadius: "8px",
                        fontSize: "14px", cursor: "pointer",
                        touchAction: "manipulation",
                        WebkitTapHighlightColor: "transparent",
                    }}
                >
                    返回
                </button>
            </div>

            {/* iOS 权限提示 */}
            {permissionNeeded && (
                <div style={{fontSize: "11px", opacity: 0.4, marginTop: "8px"}}>
                    iOS: 请在系统设置中允许动作传感权限
                </div>
            )}
        </div>
    );
};

export default GravityControlPage;
