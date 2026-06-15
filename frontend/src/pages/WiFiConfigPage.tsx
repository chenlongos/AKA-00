import React, {useEffect, useState} from "react";
import Header from "../components/Header";
import AlertDialog from "../components/AlertDialog";
import {useViewportScale} from "../hooks/useViewportScale";

interface WifiNetwork { id: string; ssid: string; signal: number; secured: boolean; is_connected: boolean; }

const WiFiConfigPage = () => {
    const {scalePx} = useViewportScale();
    const [networks, setNetworks] = useState<WifiNetwork[]>([]);
    const [connectionStatus, setConnectionStatus] = useState<React.ReactNode>("正在获取状态...");
    const [expandedId, setExpandedId] = useState<string | null>(null);
    const [scanning, setScanning] = useState(false);
    const [connecting, setConnecting] = useState(false);
    const [passwords, setPasswords] = useState<Record<string, string>>({});
    const [messages, setMessages] = useState<Record<string, string>>({});
    const [alertMsg, setAlertMsg] = useState("");

    useEffect(() => { loadList(); }, []);

    const getBars = (signal: number) => {
        const level = signal > -60 ? 4 : signal > -70 ? 3 : signal > -80 ? 2 : 1;
        return (
            <div style={{display: "flex", alignItems: "flex-end", gap: 2, height: 12}}>
                {[1, 2, 3, 4].map(i => (
                    <div key={i} style={{width: scalePx(3), background: i <= level ? "#34c759" : "var(--color-bg-elevated)", borderRadius: 1, height: `${i * 25}%`}} />
                ))}
            </div>
        );
    };

    const updateStatus = async () => {
        try {
            const [sr, ir] = await Promise.all([fetch("/status"), fetch("/ip")]);
            const [sd, id] = await Promise.all([sr.json(), ir.json()]);
            setConnectionStatus(sd.ssid
                ? <span><span style={{color: "#34c759"}}>●</span> 已连接: <b>{sd.ssid}</b> | IP: <b style={{color: "#007aff"}}>{id.ip}</b></span>
                : <span><span style={{color: "#ff3b30"}}>○</span> 未连接 | IP: <b>{id.ip}</b></span>);
        } catch {}
    };

    const loadList = async () => {
        if (scanning || connecting) return;
        setScanning(true);
        try { const r = await fetch("/scan"); const d = await r.json(); setNetworks(d.list || []); await updateStatus(); }
        finally { setScanning(false); }
    };

    const connect = async (net: WifiNetwork) => {
        const pwd = net.secured ? passwords[net.id] || "" : "";
        if (net.secured && !pwd) { setAlertMsg("请输入密码"); return; }
        setConnecting(true);
        setMessages(prev => ({...prev, [net.id]: "正在连接并获取地址..."}));
        try {
            const r = await fetch("/connect", {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify({ssid: net.ssid, password: pwd})});
            const [status, info] = (await r.text()).split("|");
            setMessages(prev => ({...prev, [net.id]: info}));
            if (status === "success") setTimeout(loadList, 2500);
        } catch { setMessages(prev => ({...prev, [net.id]: "请求超时"})); }
        finally { setConnecting(false); }
    };

    return (
        <div style={{
            minHeight: "100%", animation: "fadeIn 0.25s ease-out",
            // 恢复原始浅色主题（WiFi 页面原本是 iOS 风格浅色）
            "--color-bg": "#f2f2f7",
            "--color-bg-card": "#ffffff",
            "--color-bg-elevated": "#e5e5ea",
            "--color-text": "#1c1c1e",
            "--color-text-dim": "#8e8e93",
            "--color-text-muted": "#6e6e73",
            "--color-border": "#d1d1d6",
            "--color-border-light": "rgba(0,0,0,0.08)",
            "--color-primary": "#007aff",
            color: "#1c1c1e",
            background: "#f2f2f7",
        } as React.CSSProperties}>
            <Header
                title="无线局域网"
                subtitle={connectionStatus}
                showClose closeTo="/settings"
                actions={
                    <span onClick={loadList}
                          style={{color: scanning || connecting ? "var(--color-text-dim)" : "var(--color-primary)", fontSize: scalePx(14), fontWeight: 600, cursor: scanning || connecting ? "not-allowed" : "pointer", userSelect: "none"}}>
                        {scanning ? "刷新中..." : "刷新扫描"}
                    </span>
                }
            />

            <div style={{padding: scalePx(12)}}>
                {networks.length === 0 ? (
                    <div style={{textAlign: "center", padding: scalePx(36), color: "var(--color-text-dim)"}}>{scanning ? "正在扫描..." : "未发现网络"}</div>
                ) : networks.map(w => (
                    <div key={w.id} style={{
                        background: "var(--color-bg-card)", borderRadius: "var(--radius-md)", marginBottom: scalePx(10),
                        boxShadow: "0 1px 2px rgba(0,0,0,0.1)", overflow: "hidden",
                        border: w.is_connected ? "2px solid var(--color-success)" : "2px solid transparent",
                    }}>
                        <div onClick={() => setExpandedId(expandedId === w.id ? null : w.id)}
                             style={{padding: `${scalePx(12)} ${scalePx(14)}`, display: "flex", justifyContent: "space-between", alignItems: "center", cursor: "pointer"}}>
                            <div style={{display: "flex", flexDirection: "column"}}>
                                <div style={{fontSize: scalePx(15), fontWeight: 500, display: "flex", alignItems: "center", gap: 6}}>
                                    {w.ssid}
                                    {w.is_connected && <span style={{background: "var(--color-success)", color: "#fff", fontSize: scalePx(9), padding: `${scalePx(1)} ${scalePx(5)}`, borderRadius: "var(--radius-sm)", fontWeight: 600}}>已连接</span>}
                                </div>
                                <div style={{fontSize: scalePx(11), color: "var(--color-text-dim)", marginTop: 2}}>{w.secured ? "🔒 安全" : "🔓 公开"} · {w.signal} dBm</div>
                            </div>
                            {getBars(w.signal)}
                        </div>
                        {expandedId === w.id && (
                            <div style={{padding: `0 ${scalePx(14)} ${scalePx(14)}`, background: "var(--color-bg)", borderTop: "1px solid var(--color-border-light)"}}>
                                {w.secured ? (
                                    <input type="password" placeholder="输入密码" value={passwords[w.id] || ""}
                                           onChange={e => setPasswords(prev => ({...prev, [w.id]: e.target.value}))}
                                           style={{width: "100%", padding: scalePx(10), border: "1px solid var(--color-border)", borderRadius: "var(--radius-sm)", margin: `${scalePx(10)} 0`, fontSize: scalePx(14), boxSizing: "border-box", outline: "none", background: "var(--color-bg-card)", color: "var(--color-text)"}} />
                                ) : (
                                    <div style={{fontSize: scalePx(12), color: "var(--color-text-dim)", padding: `${scalePx(8)} 0`}}>此网络无需密码</div>
                                )}
                                <button onClick={() => connect(w)} disabled={connecting}
                                        style={{width: "100%", padding: scalePx(10), border: "none", borderRadius: "var(--radius-sm)", background: connecting ? "var(--color-text-dim)" : "var(--color-primary)", color: "#fff", fontSize: scalePx(15), fontWeight: 600, cursor: connecting ? "not-allowed" : "pointer", outline: "none"}}>
                                    连接网络
                                </button>
                                {messages[w.id] && <div style={{fontSize: scalePx(12), textAlign: "center", marginTop: scalePx(8), color: messages[w.id].includes("成功") ? "#34c759" : "#ff3b30"}}>{messages[w.id]}</div>}
                            </div>
                        )}
                    </div>
                ))}
            </div>

            <AlertDialog open={!!alertMsg} message={alertMsg} onClose={() => setAlertMsg("")} />
        </div>
    );
};

export default WiFiConfigPage;
