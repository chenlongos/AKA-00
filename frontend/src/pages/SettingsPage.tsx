import {useEffect, useState} from "react";
import {useNavigate} from "react-router-dom";
import Page from "../components/Page";
import Card from "../components/Card";
import {S} from "../styles";
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
