import {useEffect, useRef, useState} from "react";
import {controlSocket} from "../api";
import ControlButton from "../components/ControlButton.tsx";
import CameraToggle from "../components/CameraToggle";
import FullscreenButton from "../components/FullscreenButton";
import Page from "../components/Page";
import AlertDialog from "../components/AlertDialog";
import {S} from "../styles";
import {useViewportScale} from "../hooks/useViewportScale";

const BaseControlPage = () => {
    const {scalePx, scale} = useViewportScale();
    const [ip, setIp] = useState("获取中...");
    const [status, setStatus] = useState("准备就绪");
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [wsReady, setWsReady] = useState(false);
    const [isLandscape, setIsLandscape] = useState(() => window.innerWidth > window.innerHeight);
    const [alertMsg, setAlertMsg] = useState("");
    const currentActionRef = useRef<string | null>(null);

    useEffect(() => {
        const check = () => setIsLandscape(window.innerWidth > window.innerHeight);
        window.addEventListener("resize", check);
        window.addEventListener("orientationchange", check);
        return () => { window.removeEventListener("resize", check); window.removeEventListener("orientationchange", check); };
    }, []);

    useEffect(() => {
        controlSocket.connect(
            undefined,
            (s) => {
                setLeftSpeed(s.left);
                setRightSpeed(s.right);
                // 每次收到 0xBB 帧就刷新活跃时间戳（online 检测用）
                lastMotorUpdateRef.current = Date.now();
            },
            (msg) => {
                if (msg.type === "ip") {
                    setIp("IP: " + msg.ip);
                    setStatus("准备就绪");
                    setWsReady(true);
                }
            },
        );
        return () => { controlSocket.close(); };
    }, []);

    // online 状态用 motor_status 活跃度判断（dora 推 0xBB 帧 200ms 一次）。
    // 比 heartbeat HTTP 轮询更可靠：实时、不依赖前端 fetch、由 dora 推动。
    // 3s 内有 0xBB 帧就绿，否则红（dora 死了 / wifi 断了 / web-server 卡了都触发）。
    useEffect(() => {
        const tick = window.setInterval(() => {
            const since = Date.now() - lastMotorUpdateRef.current;
            if (lastMotorUpdateRef.current === 0 || since > 3000) {
                setOnline(false);
            } else {
                setOnline(true);
            }
        }, 1000);
        return () => window.clearInterval(tick);
    }, []);



    const [online, setOnline] = useState(false);
    const lastMotorUpdateRef = useRef<number>(0);

    // WebSocket 就绪后执行初始化（sendRawCommand 依赖 WS 连接）
    useEffect(() => {
        if (!wsReady) return;
        const processHash = () => {
            const hash = window.location.hash;
            if (hash) {
                controlSocket.sendRawCommand(hash);
                window.history.replaceState(null, document.title, window.location.pathname);
            }
        };
        processHash();
        window.addEventListener('hashchange', processHash);
        controlSocket.sendPwmChannels();
        controlSocket.sendReinitialize();
        return () => window.removeEventListener('hashchange', processHash);
    }, [wsReady]);

    const send = (action: string, speed: number = 50) => {
        setStatus("执行: " + action);
        if (action === "stop") { controlSocket.sendJoystick(0, 0); }
        else if (action === "up") { controlSocket.sendJoystick(0, speed); }
        else if (action === "down") { controlSocket.sendJoystick(0, -speed); }
        else if (action === "left") { controlSocket.sendJoystick(-speed, 0); }
        else if (action === "right") { controlSocket.sendJoystick(speed, 0); }
        else { controlSocket.sendAction(action, speed); }
    };

    const handlePressStart = (action: string, speed?: number) => { currentActionRef.current = action; send(action, speed); };
    const handlePressEnd = () => { currentActionRef.current = null; send("stop"); };

    const redirect = () => {
        const ipStr = ip.replace("IP: ", "");
        if (!ipStr || ipStr === "获取中...") { setAlertMsg("无法获取IP，请稍后重试"); return; }
        window.location.replace(`https://labs.chenlongrobot.com/?ip=${encodeURIComponent(ipStr)}`);
    };

    // 十字方向键尺寸 — 竖屏取宽度，横屏取高度
    const dpadSize = isLandscape
        ? Math.min(window.innerHeight * 0.8, window.innerWidth * 0.45)
        : Math.min(window.innerWidth * 0.72, Math.round(280 * scale), window.innerWidth - Math.max(60, window.innerWidth * 0.08));
    const dpadGap = Math.round(6 * scale);
    const cellSize = Math.round((dpadSize - dpadGap * 2) / 3);
    const btnFontSize = Math.round(16 * scale);
    const contentW = Math.min(window.innerWidth - Math.max(60, window.innerWidth * 0.08), Math.round(400 * scale));
    const dpadBtn = (dir: string, label: string, bgColor: string) => (
        <button
            onPointerDown={(e) => { e.preventDefault(); e.stopPropagation(); handlePressStart(dir, dir === "left" || dir === "right" ? 50 : 50); }}
            onPointerUp={(e) => { e.preventDefault(); handlePressEnd(); }}
            onPointerLeave={handlePressEnd}
            onPointerCancel={handlePressEnd}
            style={{
                width: cellSize, height: cellSize,
                borderRadius: scalePx(18),  // 匹配原始 ControlButton 圆角
                border: "none",
                background: bgColor, color: "#fff",
                fontWeight: "bold", fontSize: btnFontSize,
                touchAction: "none",
                display: "flex", alignItems: "center", justifyContent: "center",
                boxShadow: "0 6px 15px rgba(0,0,0,0.5)",  // 匹配原始 ControlButton 阴影
                cursor: "pointer", outline: "none",
                transition: "all 0.15s ease",
            }}
        >
            {label}
        </button>
    );

    // 公共子组件
    const Dpad = () => (
        <div style={{display: "grid", gridTemplateColumns: `repeat(3, ${cellSize}px)`, gridTemplateRows: `repeat(3, ${cellSize}px)`, gap: dpadGap, width: dpadSize, height: dpadSize, flexShrink: 0}}>
            <div />{dpadBtn("up", "▲", "#2563eb")}<div />
            {dpadBtn("left", "◀", "#2563eb")}
            <button onPointerDown={(e) => { e.preventDefault(); handlePressStart("stop"); }}
                    style={{width: cellSize, height: cellSize, borderRadius: scalePx(18), border: "none", background: "#ef4444", color: "#fff", fontWeight: "bold", fontSize: Math.round(12 * scale), touchAction: "none", display: "flex", alignItems: "center", justifyContent: "center", boxShadow: "0 6px 15px rgba(0,0,0,0.5)", cursor: "pointer", outline: "none", letterSpacing: "1px", transition: "all 0.15s ease"}}>
                停止
            </button>
            {dpadBtn("right", "▶", "#2563eb")}
            <div />{dpadBtn("down", "▼", "#2563eb")}<div />
        </div>
    );

    const Speed = () => (
        <div style={{display: "flex", justifyContent: "center"}}>
            <div style={{display: "flex", gap: scalePx(20), padding: `${scalePx(6)} ${scalePx(16)}`, background: "var(--color-bg-card)", borderRadius: "var(--radius-full)", fontSize: scalePx(12)}}>
                <span>左 <b style={S.success}>{leftSpeed.toFixed(1)}</b> m/s</span>
                <span>右 <b style={S.success}>{rightSpeed.toFixed(1)}</b> m/s</span>
            </div>
        </div>
    );

    const SideButtons = () => (
        <div style={{display: "flex", flexDirection: "column", gap: scalePx(8), width: "100%", flexShrink: 0}}>
            <div style={{display: "flex", gap: scalePx(10), width: "100%"}}>
                <ControlButton variant="success" size="full" onClick={() => send("grab")}>🤏 抓取</ControlButton>
                <ControlButton variant="secondary" size="full" onClick={() => send("release")}>👐 释放</ControlButton>
            </div>
            <div style={{display: "flex", gap: scalePx(10), width: "100%"}}>
                <ControlButton size="full" variant="primary" onClick={() => window.location.href = "/gravity"}>📱 重力遥控</ControlButton>
                <ControlButton size="full" variant="secondary" onClick={() => redirect()}>🧪 试验平台</ControlButton>
            </div>
        </div>
    );

    // ====== 横屏 ======
    if (isLandscape) {
        return (
            <Page center padTop={6}>
                <div style={{display: "flex", alignItems: "stretch", height: "100%", width: "100%", gap:"30px"}}>
                    {/* 左：方向键 */}
                    <div style={{display: "flex", alignItems: "center", justifyContent: "center", flex: 1, width: "100%", height: "100%"}}>
                        <Dpad />
                    </div>

                    {/* 右：信息 + 按钮，宽度对齐方向键 */}
                    <div style={{display: "flex", alignItems: "center", justifyContent: "center", flex: 1, width: "100%", height: "100%"}}>
                        <div style={{display: "flex", flexDirection: "column", gap: scalePx(8), justifyContent: "center", width: dpadSize}}>
                            <div style={{textAlign: "center", width: "100%"}}>
                                <h2 style={{fontSize: scalePx(15), fontWeight: 700, margin: 0}}>辰龙机器人控制台</h2>
                                <div style={{
                                    fontSize: scalePx(13), fontWeight: 500,
                                    color: "var(--color-text-muted)", marginTop: 2,
                                    fontFamily: "monospace",
                                    userSelect: "text", WebkitUserSelect: "text",
                                    WebkitTouchCallout: "default",
                                }}>{ip}</div>
                            </div>

                            <div style={{...S.rowBetween, width: "100%"}}>
                                <div style={{...S.row, gap: scalePx(6)}}>
                                    <span style={S.dot(online)} />
                                    <span style={S.muted}>{status}</span>
                                </div>
                                <div style={{...S.row, gap: scalePx(6)}}>
                                    <FullscreenButton />
                                    <span style={S.muted}>摄像头</span>
                                    <CameraToggle />
                                </div>
                            </div>

                            <div style={{width: "100%"}}><Speed /></div>
                            <div style={{width: "100%", flex: 1}}><SideButtons /></div>
                        </div>
                    </div>
                </div>
                <AlertDialog open={!!alertMsg} message={alertMsg} onClose={() => setAlertMsg("")} />
            </Page>
        );
    }

    // ====== 竖屏 ======
    return (
        <Page center padTop={10}>
            <div style={{textAlign: "center", width: "100%"}}>
                <h2 style={{fontSize: scalePx(17), fontWeight: 700, margin: 20}}>辰龙机器人控制台</h2>
                <div style={{
                    fontSize: scalePx(15), fontWeight: 500,
                    color: "var(--color-text-muted)", marginTop: scalePx(4),
                    fontFamily: "monospace",
                    userSelect: "text", WebkitUserSelect: "text",
                    WebkitTouchCallout: "default",
                }}>{ip}</div>
            </div>

            <div style={{...S.rowBetween, width: "100%", maxWidth: contentW, marginTop: scalePx(8)}}>
                <div style={{...S.row, gap: scalePx(6)}}>
                    <span style={S.dot(online)} />
                    <span style={S.muted}>{status}</span>
                </div>
                <div style={{...S.row, gap: scalePx(6)}}>
                    <FullscreenButton />
                    <span style={S.muted}>摄像头</span>
                    <CameraToggle />
                </div>
            </div>

            <div style={{marginTop: scalePx(12)}}><Dpad /></div>
            <div style={{marginTop: scalePx(10), width: "100%", maxWidth: contentW}}><Speed /></div>
            <div style={{marginTop: scalePx(12), width: "100%", maxWidth: contentW}}><SideButtons /></div>
            <AlertDialog open={!!alertMsg} message={alertMsg} onClose={() => setAlertMsg("")} />
        </Page>
    );
};

export default BaseControlPage;
