import {useEffect, useState} from "react";
import {useNavigate} from "react-router-dom";
import Page from "../components/Page";
import Card from "../components/Card";
import {S} from "../styles";
import {api} from "../api";
import {useViewportScale} from "../hooks/useViewportScale";

const SettingsPage = () => {
    const {scalePx} = useViewportScale();
    const navigate = useNavigate();
    const [devInfo, setDevInfo] = useState({ip: "加载中...", mac: "加载中..."});

    useEffect(() => {
        fetch("/api/system/info")
            .then(r => r.json())
            .then(d => setDevInfo({ip: d.ip || "未知", mac: d.mac || "未知"}))
            .catch(() => setDevInfo({ip: "获取失败", mac: "获取失败"}));
    }, []);

    // 屏幕显示开关：全屏写屏很吃那颗单核 CPU（实测 /api/detect 从 120ms 涨到 340ms），
    // 想让它跑得快就关掉；只是运行时开关，重启后回到 config.toml 的值。
    const [screenOn, setScreenOn] = useState<boolean | null>(null);
    const [screenBusy, setScreenBusy] = useState(false);

    useEffect(() => {
        if (!__WITH_SCREEN__) return;   // 不带屏版：连问都不问（这块整个不存在）
        api.display.status().then(d => setScreenOn(!!d.enabled)).catch(() => setScreenOn(null));
    }, []);

    const toggleScreen = () => {
        if (screenBusy || screenOn === null) return;
        setScreenBusy(true);
        api.display.setEnabled(!screenOn)
            .then(d => { if (typeof d.enabled === "boolean") setScreenOn(d.enabled); })
            .catch(() => {})
            .finally(() => setScreenBusy(false));
    };

    const items = [
        {icon: "📶", title: "WiFi 配置", desc: "扫描并连接无线网络", path: "/wifi"},
        {icon: "🔧", title: "舵机角度配置", desc: "调整抓取序列", path: "/arm-angles"},
        {icon: "🚗", title: "行驶速度", desc: "调整前进和转向速度百分比", path: "/speed-config"},
        {icon: "⬆️", title: "固件升级", desc: "检查更新并升级系统固件", path: "/ota"},
    ];

    return (
        <Page center>
            <h2 style={{fontSize: scalePx(17), fontWeight: 700, marginBottom: scalePx(16), marginTop: "20px"}}>设置</h2>
            <div style={{width: "100%", maxWidth: scalePx(420)}}>
                {items.map(item => (
                    <Card key={item.path} marginBottom={10}>
                        <div
                            onClick={() => navigate(item.path)}
                            style={{
                                display: "flex", alignItems: "center", gap: scalePx(12),
                                padding: `${scalePx(4)} 0`, cursor: "pointer",
                            }}
                        >
                            <span style={{fontSize: scalePx(24)}}>{item.icon}</span>
                            <div style={{flex: 1}}>
                                <div style={{fontSize: scalePx(15), fontWeight: 600, color: "var(--color-text)"}}>
                                    {item.title}
                                </div>
                                <div style={{...S.muted, marginTop: 2}}>{item.desc}</div>
                            </div>
                            <span style={{fontSize: scalePx(16), color: "var(--color-text-dim)"}}>›</span>
                        </div>
                    </Card>
                ))}

                {/* 屏幕显示开关 —— **不带屏版整块不存在**（不是"显示了再隐藏"：
                    没有屏的机器上，用户不该看到一个拨了没反应的开关） */}
                {__WITH_SCREEN__ && <Card marginBottom={10}>
                    <div style={{display: "flex", alignItems: "center", gap: scalePx(12), padding: `${scalePx(4)} 0`}}>
                        <span style={{fontSize: scalePx(24)}}>🖥️</span>
                        <div style={{flex: 1}}>
                            <div style={{fontSize: scalePx(15), fontWeight: 600, color: "var(--color-text)"}}>
                                屏幕显示
                            </div>
                            <div style={{...S.muted, marginTop: 2}}>
                                关掉可让检测/追物更快（写屏会占用单核 CPU）
                            </div>
                        </div>
                        <div
                            onClick={toggleScreen}
                            style={{
                                width: scalePx(44), height: scalePx(24), borderRadius: scalePx(12),
                                background: screenOn ? "var(--color-success)" : "var(--color-bg-elevated)",
                                cursor: (screenBusy || screenOn === null) ? "wait" : "pointer",
                                opacity: (screenBusy || screenOn === null) ? 0.6 : 1,
                                position: "relative", transition: "background 0.2s, opacity 0.15s", flexShrink: 0,
                            }}
                        >
                            <div style={{
                                position: "absolute", top: "50%", transform: "translateY(-50%)",
                                left: screenOn ? scalePx(24) : scalePx(3),
                                width: scalePx(18), height: scalePx(18), borderRadius: "50%", background: "white",
                                transition: "left 0.2s ease", boxShadow: "0 1px 3px rgba(0,0,0,0.3)",
                            }}/>
                        </div>
                    </div>
                </Card>}

                {/* 设备信息 */}
                <Card marginBottom={10}>
                    <div style={{fontSize: scalePx(13), color: "var(--color-text-dim)"}}>
                        <div style={{display: "flex", justifyContent: "space-between", marginBottom: scalePx(4)}}>
                            <span>IP 地址</span>
                            <span style={{fontFamily: "monospace", color: "var(--color-text)"}}>{devInfo.ip}</span>
                        </div>
                        <div style={{display: "flex", justifyContent: "space-between"}}>
                            <span>MAC 地址</span>
                            <span style={{fontFamily: "monospace", color: "var(--color-text)"}}>{devInfo.mac}</span>
                        </div>
                    </div>
                </Card>
            </div>
        </Page>
    );
};

export default SettingsPage;
