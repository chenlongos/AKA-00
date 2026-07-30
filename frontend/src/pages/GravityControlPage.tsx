import {useViewportScale} from "../hooks/useViewportScale";
import {useState, useEffect, useRef, useCallback} from "react";
import {useNavigate} from "react-router-dom";
import {api} from "../api";

const DEADZONE = 18;
const SPEED_LR = 25;

const cmdLabel: Record<string, string> = {
    up: "前进", down: "后退", left: "左转", right: "右转", stop: "停止",
};

const GravityControlPage = () => {
    const {scalePx} = useViewportScale();
    const navigate = useNavigate();
    const isSecure = typeof window !== "undefined" && window.isSecureContext;
    const httpsUrl = typeof window !== "undefined"
        ? window.location.href.replace(/^http:/, "https:").replace(/:80(\/|$)/, ":443$1")
        : "";
    const [pitch, setPitch] = useState(0);
    const [roll, setRoll] = useState(0);
    const [cmd, setCmd] = useState("stop");
    const [active, setActive] = useState(false);
    const [permissionNeeded, setPermissionNeeded] = useState(false);
    const [showNonSecure, setShowNonSecure] = useState(!isSecure);
    const [speedFB, setSpeedFB] = useState(50);
    const cmdRef = useRef("stop");
    const speedFBRef = useRef(50);
    const dpadActiveRef = useRef(false);
    const wakeLockRef = useRef<any>(null);

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
            const pct = (nextCmd === "left" || nextCmd === "right") ? SPEED_LR : speedFBRef.current;
            const speed = Math.round(pct * 0.5 / 100 * 1000) / 1000;  // % → m/s (50% → 0.25 m/s → motor 50)
            api.motor.action(nextCmd, speed).catch(() => {});
        }
    }, [vibrate]);

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
        if (!isSecure) { setShowNonSecure(true); return; }
        if (typeof (DeviceOrientationEvent as any).requestPermission === "function") {
            try {
                const perm = await (DeviceOrientationEvent as any).requestPermission();
                if (perm !== "granted") return;
            } catch { setPermissionNeeded(true); return; }
        }
        setActive(true);
    };

    const stopGravity = () => setActive(false);

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
                width: "100%", height: "100%", borderRadius: "var(--radius-md)", border: "1px solid",
                display: "flex", alignItems: "center", justifyContent: "center",
                fontSize: scalePx(20),
                background: cmd === dir ? "#2563eb" : "var(--color-bg-card)",
                borderColor: cmd === dir ? "#60a5fa" : "var(--color-border)",
                color: cmd === dir ? "#fff" : "var(--color-text-dim)",
                boxShadow: cmd === dir ? "0 0 15px rgba(37,99,235,0.4)" : "none",
                transition: "all 0.15s ease",
                touchAction: "none",
                cursor: "pointer",
                outline: "none",
            }}>
            {icon}
        </button>
    );

    return (
        <div style={{
            position: "fixed", inset: 0, background: "#020617", color: "#f1f5f9",
            display: "flex", flexDirection: "column", alignItems: "center",
            userSelect: "none", overflow: "hidden", touchAction: "none",
            padding: `${scalePx(12)} ${scalePx(12)} ${scalePx(24)}`,
            zIndex: 200,
        }}>
            {/* Non-Secure overlay */}
            {!isSecure && showNonSecure && (
                <div style={{
                    position: "fixed", inset: 0, background: "rgba(2,6,23,0.95)", zIndex: 60,
                    display: "flex", alignItems: "center", justifyContent: "center",
                    padding: scalePx(32), textAlign: "center",
                }}>
                    <div style={{
                        background: "var(--color-bg)", border: "1px solid var(--color-border)",
                        padding: scalePx(28), borderRadius: "var(--radius-2xl)",
                        display: "flex", flexDirection: "column", gap: scalePx(14),
                        maxWidth: scalePx(300),
                    }}>
                        <div style={{fontSize: scalePx(44)}}>&#128683;</div>
                        <h2 style={{fontSize: scalePx(18), fontWeight: 700}}>需要 HTTPS 环境</h2>
                        <p style={{fontSize: scalePx(13), color: "var(--color-text-muted)", lineHeight: 1.6}}>
                            iOS / Android 均要求 <b>HTTPS</b> 加密连接才能启用陀螺仪重力感应。
                        </p>
                        <button onClick={() => { window.location.href = httpsUrl; }}
                                style={{background: "#2563eb", color: "#fff", border: "none", padding: `${scalePx(10)} ${scalePx(20)}`, borderRadius: "var(--radius-md)", fontSize: scalePx(15), fontWeight: 700, cursor: "pointer", touchAction: "manipulation"}}>
                            切换到 HTTPS
                        </button>
                        <button onClick={() => setShowNonSecure(false)}
                                style={{background: "transparent", color: "var(--color-text-muted)", border: "1px solid var(--color-border)", padding: `${scalePx(8)} ${scalePx(20)}`, borderRadius: "var(--radius-md)", fontSize: scalePx(13), cursor: "pointer", touchAction: "manipulation"}}>
                            继续使用手动操作
                        </button>
                    </div>
                </div>
            )}

            {!isSecure && !showNonSecure && (
                <div style={{
                    width: "100%", maxWidth: scalePx(480),
                    background: "rgba(234,88,12,0.15)", border: "1px solid rgba(234,88,12,0.3)",
                    borderRadius: "var(--radius-md)", padding: `${scalePx(8)} ${scalePx(12)}`,
                    display: "flex", alignItems: "center", justifyContent: "space-between",
                    gap: "12px", marginBottom: scalePx(6),
                }}>
                    <span style={{fontSize: scalePx(10), color: "#fb923c", flex: 1}}>非 HTTPS 环境，无法使用重力感应</span>
                    <button onClick={() => setShowNonSecure(true)}
                            style={{background: "transparent", color: "#60a5fa", border: "1px solid #3b82f6", borderRadius: "var(--radius-sm)", padding: `${scalePx(3)} ${scalePx(8)}`, fontSize: scalePx(10), fontWeight: 600, cursor: "pointer", whiteSpace: "nowrap", touchAction: "manipulation"}}>
                        切换 HTTPS
                    </button>
                </div>
            )}

            {/* Top bar */}
            <div style={{width: "100%", maxWidth: scalePx(480), display: "flex", flexDirection: "column", gap: scalePx(6), marginBottom: scalePx(12)}}>
                <div style={{display: "flex", justifyContent: "space-between", alignItems: "flex-end"}}>
                    <div style={{display: "flex", flexDirection: "column"}}>
                        <span style={{color: "#3b82f6", fontWeight: 900, fontStyle: "italic", letterSpacing: "0.1em", fontSize: scalePx(16)}}>
                            AI 智能控制
                        </span>
                        <span style={{fontSize: scalePx(9), color: "var(--color-text-dim)", fontFamily: "monospace"}}>
                            {!isSecure ? "⚠️ 非 HTTPS (陀螺仪受限)" : active ? "重力遥控已激活" : "已就绪"}
                        </span>
                    </div>
                    <div style={{background: "var(--color-bg-card)", border: "1px solid var(--color-border)", borderRadius: "var(--radius-sm)", display: "flex", alignItems: "center", padding: `${scalePx(3)} ${scalePx(7)}`}}>
                        <span style={{fontSize: scalePx(9), color: "var(--color-text-muted)", marginRight: "6px", textTransform: "uppercase"}}>速度</span>
                        <span style={{color: "#60a5fa", fontWeight: 700, fontFamily: "monospace"}}>{speedFB}</span>
                    </div>
                </div>
            </div>

            {/* HUD */}
            <div style={{flex: 1, width: "100%", position: "relative", display: "flex", alignItems: "center", justifyContent: "center"}}>
                <div style={{position: "absolute", width: scalePx(220), height: scalePx(220), border: "1px solid rgba(59,130,246,0.1)", borderRadius: "50%"}} />
                <div style={{
                    position: "relative", width: scalePx(140), height: scalePx(140),
                    border: "2px solid rgba(59,130,246,0.4)", borderRadius: "50%",
                    display: "flex", alignItems: "center", justifyContent: "center",
                    transform: `rotateX(${pitch}deg) rotateY(${roll}deg)`,
                    transition: "transform 0.1s ease-out",
                    boxShadow: "0 0 30px rgba(59,130,246,0.1)",
                }}>
                    <div style={{position: "absolute", width: "100%", height: "1px", background: "rgba(59,130,246,0.3)"}} />
                    <div style={{position: "absolute", width: "1px", height: "100%", background: "rgba(59,130,246,0.3)"}} />
                    <div style={{position: "absolute", width: scalePx(40), height: scalePx(40), display: "flex", alignItems: "center", justifyContent: "center", fontSize: scalePx(22), color: isMoving ? "#f97316" : "rgba(59,130,246,0.5)", transform: isMoving ? "scale(1.1)" : "scale(1)", transition: "all 0.3s"}}>
                        {arrowIcon}
                    </div>
                </div>
                <div style={{position: "absolute", bottom: "16px", padding: `${scalePx(3)} ${scalePx(16)}`, background: "#2563eb", borderRadius: "var(--radius-full)", fontSize: scalePx(11), fontWeight: 900, textTransform: "uppercase", letterSpacing: "0.1em", boxShadow: "0 10px 15px -3px rgba(0,0,0,0.1)"}}>
                    {cmdLabel[cmd] || "待机"}
                </div>
            </div>

            {/* Control panel */}
            <div style={{width: "100%", maxWidth: scalePx(480), display: "grid", gridTemplateColumns: "1fr 1fr", gap: scalePx(14)}}>
                <div style={{display: "flex", flexDirection: "column", gap: scalePx(14)}}>
                    <div style={{background: "var(--color-bg)", padding: scalePx(10), borderRadius: "var(--radius-lg)", border: "1px solid var(--color-border)"}}>
                        <span style={{fontSize: scalePx(9), color: "var(--color-text-dim)", display: "block", marginBottom: scalePx(6), fontWeight: 700, textTransform: "uppercase"}}>速度设定</span>
                        <input type="range" min="30" max="70" value={speedFB} onChange={(e) => setSpeedFB(parseInt(e.target.value))}
                               style={{width: "100%", height: scalePx(20), accentColor: "var(--color-primary)", margin: 0}} />
                    </div>
                    <button onClick={active ? stopGravity : startGravity}
                            style={{display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", padding: scalePx(14), borderRadius: "var(--radius-lg)", border: "1px solid", background: active ? "#ea580c" : "var(--color-bg)", borderColor: active ? "#fb923c" : "var(--color-border)", color: active ? "#fff" : "var(--color-text-muted)", boxShadow: active ? "0 10px 15px -3px rgba(0,0,0,0.1)" : "none", transition: "all 0.15s ease", cursor: "pointer", touchAction: "manipulation"}}>
                        <span style={{fontSize: scalePx(18), marginBottom: scalePx(2)}}>{active ? "📡 开启中" : "📴 重力感应"}</span>
                        <span style={{fontSize: scalePx(9), fontWeight: 700, textTransform: "uppercase"}}>点此切换模式</span>
                    </button>
                    <div style={{display: "grid", gridTemplateColumns: "1fr 1fr", gap: scalePx(7)}}>
                        <button onPointerDown={(e) => { e.preventDefault(); sendCmd("grab"); }}
                                onPointerUp={(e) => { e.preventDefault(); sendCmd("stop"); }}
                                onPointerLeave={() => sendCmd("stop")}
                                style={{background: "var(--color-bg)", border: "1px solid var(--color-border)", padding: scalePx(10), borderRadius: "var(--radius-md)", display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", color: "var(--color-text-muted)", cursor: "pointer", touchAction: "none"}}>
                            <span style={{fontSize: scalePx(16)}}>🤏</span>
                            <span style={{fontSize: scalePx(7), fontWeight: 700, textTransform: "uppercase", color: "var(--color-text-dim)", marginTop: "2px"}}>抓取</span>
                        </button>
                        <button onPointerDown={(e) => { e.preventDefault(); sendCmd("release"); }}
                                onPointerUp={(e) => { e.preventDefault(); sendCmd("stop"); }}
                                onPointerLeave={() => sendCmd("stop")}
                                style={{background: "var(--color-bg)", border: "1px solid var(--color-border)", padding: scalePx(10), borderRadius: "var(--radius-md)", display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", color: "var(--color-text-muted)", cursor: "pointer", touchAction: "none"}}>
                            <span style={{fontSize: scalePx(16)}}>👐</span>
                            <span style={{fontSize: scalePx(7), fontWeight: 700, textTransform: "uppercase", color: "var(--color-text-dim)", marginTop: "2px"}}>释放</span>
                        </button>
                    </div>
                </div>

                <div style={{aspectRatio: "1", background: "rgba(15,23,42,0.5)", borderRadius: "var(--radius-2xl)", padding: scalePx(10), border: "1px solid var(--color-border)", display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gridTemplateRows: "1fr 1fr 1fr", gap: scalePx(6)}}>
                    <div style={{gridColumn: "2", gridRow: "1"}}>{dpadBtn("up", "▲")}</div>
                    <div style={{gridColumn: "1", gridRow: "2"}}>{dpadBtn("left", "◀")}</div>
                    <div style={{gridColumn: "2", gridRow: "2"}}>
                        <button onPointerDown={(e) => { e.preventDefault(); handleDPadDown("stop"); }}
                                style={{width: "100%", height: "100%", borderRadius: "var(--radius-md)", border: "1px solid rgba(239,68,68,0.3)", background: "rgba(239,68,68,0.1)", color: "#ef4444", fontSize: scalePx(9), fontWeight: 900, touchAction: "none", cursor: "pointer", outline: "none"}}>
                            停止
                        </button>
                    </div>
                    <div style={{gridColumn: "3", gridRow: "2"}}>{dpadBtn("right", "▶")}</div>
                    <div style={{gridColumn: "2", gridRow: "3"}}>{dpadBtn("down", "▼")}</div>
                </div>
            </div>

            {/* Back */}
            <div style={{marginTop: scalePx(12)}}>
                <button onClick={() => { stopGravity(); navigate("/"); }}
                        style={{background: "rgba(255,255,255,0.1)", border: "1px solid var(--color-border)", color: "#fff", padding: `${scalePx(7)} ${scalePx(20)}`, borderRadius: "var(--radius-sm)", fontSize: scalePx(13), cursor: "pointer", touchAction: "manipulation", outline: "none"}}>
                    返回
                </button>
            </div>

            {permissionNeeded && (
                <div style={{fontSize: scalePx(10), opacity: 0.4, marginTop: scalePx(6)}}>iOS: 请在系统设置中允许动作传感权限</div>
            )}
        </div>
    );
};

export default GravityControlPage;
