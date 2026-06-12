import {useViewportScale} from "../hooks/useViewportScale";
import React, {useEffect, useState} from "react";

interface WifiNetwork {
    id: string;
    ssid: string;
    signal: number;
    secured: boolean;
    is_connected: boolean;
}

const WiFiConfigPage = () => {
    const {scalePx} = useViewportScale();
    const [networks, setNetworks] = useState<WifiNetwork[]>([]);
    const [connectionStatus, setConnectionStatus] = useState<React.ReactNode>("正在获取状态...");
    const [expandedId, setExpandedId] = useState<string | null>(null);
    const [scanning, setScanning] = useState(false);
    const [connecting, setConnecting] = useState(false);
    const [passwords, setPasswords] = useState<Record<string, string>>({});
    const [messages, setMessages] = useState<Record<string, string>>({});

    useEffect(() => {
        loadList();
    }, []);

    const getBars = (signal: number) => {
        const level = signal > -60 ? 4 : signal > -70 ? 3 : signal > -80 ? 2 : 1;
        const bars = [];
        for (let i = 1; i <= 4; i++) {
            bars.push(
                <div
                    key={i}
                    style={{
                        width: scalePx(3),
                        background: i <= level ? "#34c759" : "#e5e5ea",
                        borderRadius: scalePx(0.5),
                        height: `${i * 25}%`,
                    }}
                />
            );
        }
        return <div style={{display: "flex", alignItems: "flex-end", gap: scalePx(2), height: 12}}>{bars}</div>;
    };

    const updateStatus = async () => {
        try {
            const [statusRes, ipRes] = await Promise.all([fetch("/status"), fetch("/ip")]);
            const statusData = await statusRes.json();
            const ipData = await ipRes.json();
            if (statusData.ssid) {
                setConnectionStatus(
                    <span>
                        <span style={{color: "#34c759"}}>●</span> 已连接: <b>{statusData.ssid}</b> | IP:{" "}
                        <b style={{color: "#007aff"}}>{ipData.ip}</b>
                    </span>
                );
            } else {
                setConnectionStatus(
                    <span>
                        <span style={{color: "#ff3b30"}}>○</span> 未连接 | IP: <b>{ipData.ip}</b>
                    </span>
                );
            }
            // eslint-disable-next-line @typescript-eslint/no-unused-vars
        } catch (_) {
            console.error("无法获取状态");
        }
    };

    const loadList = async () => {
        if (scanning || connecting) return;

        setScanning(true);
        try {
            const r = await fetch("/scan");
            const data = await r.json();
            setNetworks(data.list || []);
            await updateStatus();
        } finally {
            setScanning(false);
        }
    };

    const toggle = (id: string) => {
        setExpandedId(expandedId === id ? null : id);
    };

    const handlePasswordChange = (id: string, value: string) => {
        setPasswords((prev) => ({...prev, [id]: value}));
    };

    const connect = async (network: WifiNetwork) => {
        const pwd = network.secured ? passwords[network.id] || "" : "";

        if (network.secured && !pwd) {
            alert("请输入密码");
            return;
        }

        setConnecting(true);
        setMessages((prev) => ({...prev, [network.id]: "正在连接并获取地址..."}));

        try {
            const r = await fetch("/connect", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({ssid: network.ssid, password: pwd}),
            });
            const res = await r.text();
            const [status, info] = res.split("|");
            setMessages((prev) => ({
                ...prev,
                [network.id]: info,
            }));
            if (status === "success") {
                setTimeout(loadList, 2500);
            }
            // eslint-disable-next-line @typescript-eslint/no-unused-vars
        } catch (_) {
            setMessages((prev) => ({...prev, [network.id]: "请求超时"}));
        } finally {
            setConnecting(false);
        }
    };

    return (
        <div
            style={{
                fontFamily: "-apple-system, sans-serif",
                background: "#f2f2f7",
                height: "100dvh",
                color: "#1c1c1e",
                overflowY: "auto",
                overscrollBehavior: "contain",
            }}
        >
            {/* Header */}
            <div
                style={{
                    background: "#fff",
                    padding: `${scalePx(16)} ${scalePx(20)}`,
                    borderBottom: "0.5px solid #c6c6c8",
                    display: "flex",
                    justifyContent: "space-between",
                    alignItems: "center",
                    position: "sticky",
                    top: 0,
                    zIndex: 100,
                }}
            >
                <div style={{display: "flex", flexDirection: "column"}}>
                    <h1 style={{fontSize: scalePx(17), margin: 0, fontWeight: 600}}>无线局域网</h1>
                    <div style={{fontSize: scalePx(12), color: "#8e8e93", marginTop: 2}}>{connectionStatus}</div>
                </div>
                <div
                    onClick={loadList}
                    style={{
                        color: scanning || connecting ? "#8e8e93" : "#007aff",
                        fontSize: scalePx(15),
                        cursor: scanning || connecting ? "not-allowed" : "pointer",
                        userSelect: "none",
                    }}
                >
                    {scanning ? "刷新中..." : "刷新扫描"}
                </div>
            </div>

            {/* List */}
            <div style={{padding: 16}}>
                {networks.length === 0 ? (
                    <div style={{textAlign: "center", padding: scalePx(40), color: "#8e8e93"}}>
                        {scanning ? "正在扫描..." : "未发现网络"}
                    </div>
                ) : (
                    networks.map((w) => (
                        <div
                            key={w.id}
                            style={{
                                background: "#fff",
                                borderRadius: scalePx(10),
                                marginBottom: scalePx(12),
                                boxShadow: "0 1px 2px rgba(0,0,0,0.05)",
                                overflow: "hidden",
                                border: w.is_connected ? "2px solid #34c759" : "2px solid transparent",
                            }}
                        >
                            <div
                                onClick={() => toggle(w.id)}
                                style={{
                                    padding: `${scalePx(14)} ${scalePx(16)}`,
                                    display: "flex",
                                    justifyContent: "space-between",
                                    alignItems: "center",
                                    cursor: "pointer",
                                }}
                            >
                                <div style={{display: "flex", flexDirection: "column"}}>
                                    <div style={{fontSize: scalePx(16), fontWeight: 500, display: "flex", alignItems: "center", gap: 6}}>
                                        {w.ssid}
                                        {w.is_connected && (
                                            <span
                                                style={{
                                                    background: "#34c759",
                                                    color: "#fff",
                                                    fontSize: scalePx(10),
                                                    padding: `${scalePx(2)} ${scalePx(6)}`,
                                                    borderRadius: scalePx(4),
                                                    fontWeight: 600,
                                                }}
                                            >
                                                已连接
                                            </span>
                                        )}
                                    </div>
                                    <div style={{fontSize: scalePx(12), color: "#8e8e93", marginTop: 2}}>
                                        {w.secured ? "🔒 安全" : "🔓 公开"} | {w.signal} dBm
                                    </div>
                                </div>
                                {getBars(w.signal)}
                            </div>

                            {/* Expanded box */}
                            <div
                                style={{
                                    display: expandedId === w.id ? "block" : "none",
                                    padding: `0 ${scalePx(16)} ${scalePx(16)}`,
                                    background: "#fdfdfd",
                                    borderTop: "0.5px solid #f2f2f7",
                                }}
                            >
                                {w.secured ? (
                                    <input
                                        type="password"
                                        placeholder="输入密码"
                                        value={passwords[w.id] || ""}
                                        onChange={(e) => handlePasswordChange(w.id, e.target.value)}
                                        style={{
                                            width: "100%",
                                            padding: scalePx(12),
                                            border: "1px solid #d1d1d6",
                                            borderRadius: scalePx(8),
                                            margin: `${scalePx(12)} 0`,
                                            fontSize: scalePx(16),
                                            boxSizing: "border-box",
                                            outline: "none",
                                        }}
                                    />
                                ) : (
                                    <div style={{fontSize: scalePx(13), color: "#8e8e93", padding: `${scalePx(10)} 0`}}>
                                        此网络无需密码
                                    </div>
                                )}
                                <button
                                    onClick={() => connect(w)}
                                    disabled={connecting}
                                    style={{
                                        width: "100%",
                                        padding: scalePx(12),
                                        border: "none",
                                        borderRadius: scalePx(8),
                                        background: connecting ? "#aeaeb2" : "#007aff",
                                        color: "#fff",
                                        fontSize: scalePx(16),
                                        fontWeight: 600,
                                        cursor: connecting ? "not-allowed" : "pointer",
                                    }}
                                >
                                    连接网络
                                </button>
                                {messages[w.id] && (
                                    <div
                                        style={{
                                            fontSize: scalePx(13),
                                            textAlign: "center",
                                            marginTop: scalePx(10),
                                            color: messages[w.id].includes("成功") ? "#34c759" : "#ff3b30",
                                        }}
                                    >
                                        {messages[w.id]}
                                    </div>
                                )}
                            </div>
                        </div>
                    ))
                )}
            </div>
        </div>
    );
};

export default WiFiConfigPage;
