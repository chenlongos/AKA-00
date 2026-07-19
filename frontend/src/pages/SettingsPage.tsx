import {useNavigate} from "react-router-dom";
import Page from "../components/Page";
import Card from "../components/Card";
import {S} from "../styles";
import {useViewportScale} from "../hooks/useViewportScale";

const SettingsPage = () => {
    const {scalePx} = useViewportScale();
    const navigate = useNavigate();

    const items = [
        {icon: "📶", title: "WiFi 配置", desc: "扫描并连接无线网络", path: "/wifi"},
        {icon: "🔧", title: "舵机角度配置", desc: "调整抓取序列和 PWM 通道", path: "/arm-angles"},
        {icon: "🚗", title: "行驶速度", desc: "调整前进和转向速度百分比", path: "/speed-config"},
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
            </div>
        </Page>
    );
};

export default SettingsPage;
