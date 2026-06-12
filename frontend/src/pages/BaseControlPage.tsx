import {useEffect, useRef, useState} from "react";
import {api, controlSocket} from "../api";
import ControlButton from "../components/ControlButton.tsx";
import CameraToggle from "../components/CameraToggle";
import {useViewportScale} from "../hooks/useViewportScale";


const BaseControlPage = () => {
    const {scalePx} = useViewportScale();
    const [ip, setIp] = useState("获取中...");
    const [status, setStatus] = useState("准备就绪");
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [wsConnected, setWsConnected] = useState(false);

    const currentActionRef = useRef<string | null>(null);

    useEffect(() => {
        controlSocket.connect(setWsConnected, (s) => {
            setLeftSpeed(s.left);
            setRightSpeed(s.right);
        });
        return () => { controlSocket.close(); };
    }, []);

    // URL哈希命令监听
    useEffect(() => {
        const processHash = () => {
            const hash = window.location.hash;
            if (hash) {
                api.motor.rawCommand(hash).catch(console.error);
                window.history.replaceState(null, document.title, window.location.pathname);
            }
        };

        processHash();
        window.addEventListener('hashchange', processHash);
        return () => window.removeEventListener('hashchange', processHash);
    }, []);

    useEffect(() => {
        const getIp = async () => {
            setStatus("获取 IP...");
            try {
                const data = await api.system.ip();
                setIp("IP: " + data.ip);
                setStatus("准备就绪");
            } catch {
                setStatus("获取 IP 失败");
            }
        };

        getIp();

        // 恢复PWM配置
        api.base.pwmChannels().then(data =>
            api.base.savePwmChannels(data.pwm_channels).catch(() => {})
        );

        // 重新初始化底盘（用于切换回来时重置 ESP32 状态）
        api.base.reinitialize().catch(() => {});
    }, []);

    const send = async (action: string, speed: number = 50) => {
        setStatus("执行: " + action);
        if (action === "stop") {
            controlSocket.sendJoystick(0, 0);
        } else if (action === "up") {
            controlSocket.sendJoystick(0, speed);
        } else if (action === "down") {
            controlSocket.sendJoystick(0, -speed);
        } else if (action === "left") {
            controlSocket.sendJoystick(-speed, 0);
        } else if (action === "right") {
            controlSocket.sendJoystick(speed, 0);
        } else {
            try {
                await api.motor.action(action, speed);
            } catch (err) {
                setStatus("错误: " + err);
            }
        }
    };

    const handlePressStart = (action: string, speed?: number) => {
        currentActionRef.current = action;
        send(action, speed);
    };

    const handlePressEnd = () => {
        currentActionRef.current = null;
        send("stop");
    };

    const redirect = async () => {
        setStatus("获取 IP...");
        try {
            const data = await api.system.ip();
            if (!data?.ip) throw new Error("无IP数据");
            window.location.replace(`https://labs.chenlongrobot.com/?ip=${encodeURIComponent(data.ip)}`);
        } catch {
            setStatus("跳转失败");
            alert("无法获取IP，请稍后重试");
        }
    };

    return (
        <div
            style={{
                fontFamily: "system-ui, sans-serif",
                background: "#0f172a",
                color: "white",
                minHeight: "100dvh",
                padding: scalePx(10),
                textAlign: "center",
                overflow: "hidden",
                touchAction: "none",
            }}
        >
            <h3>AKA-00 控制台</h3>
            <div style={{opacity: 0.6}}>{ip}</div>

            {/* 方向区 */}
            <div
                style={{
                    display: "flex",
                    gap: scalePx(10),
                    justifyItems: "center",
                    flexDirection: "column",
                    alignItems: "center",
                    position: "relative",
                }}
            >
                {/* 摄像头开关 */}
                <div style={{position: "absolute", top: "0", right: "0", display: "flex", alignItems: "center", gap: scalePx(6)}}>
                    <span style={{fontSize: scalePx(11), opacity: 0.6}}>摄像头</span>
                    <CameraToggle />
                </div>

                <div/>
                <ControlButton
                    onPressStart={() => handlePressStart("up")}
                    onPressEnd={() => handlePressEnd()}
                >
                    前进
                </ControlButton>
                <div style={{display: 'flex', gap: scalePx(10)}}>
                    <ControlButton
                        onPressStart={() => handlePressStart("left", 25)}
                        onPressEnd={() => handlePressEnd()}
                    >
                        左转
                    </ControlButton>
                    <ControlButton
                        variant="danger"
                        onPressStart={() => handlePressStart("stop")}
                        onPressEnd={() => handlePressEnd()}
                    >
                        停止
                    </ControlButton>
                    <ControlButton
                        onPressStart={() => handlePressStart("right", 25)}
                        onPressEnd={() => handlePressEnd()}
                    >
                        右转
                    </ControlButton>
                </div>

                <ControlButton
                    onPressStart={() => handlePressStart("down")}
                    onPressEnd={() => handlePressEnd()}
                >
                    后退
                </ControlButton>
                <div/>
            </div>


            {/* 电机实时速度显示 */}
            <div
                style={{
                    marginBottom: scalePx(20),
                    padding: scalePx(10),
                    background: "rgba(255,255,255,0.1)",
                    borderRadius: scalePx(8),
                    display: "inline-flex",
                    gap: scalePx(30),
                    fontSize: scalePx(14),
                }}
            >
                <div>左轮: <span style={{color: "#4ade80"}}>{leftSpeed}</span> m/s</div>
                <div>右轮: <span style={{color: "#4ade80"}}>{rightSpeed}</span> m/s</div>
            </div>


            {/* 功能按钮 */}
            <div
                style={{
                    display: "flex",
                    justifyContent: "center",
                    gap: scalePx(10),
                    flexWrap: "wrap",
                }}
            >
                <ControlButton
                    variant="success"
                    size="wide"
                    onClick={() => send("grab")}
                >
                    抓取
                </ControlButton>

                <ControlButton
                    variant="secondary"
                    size="wide"
                    onClick={() => send("release")}
                >
                    释放
                </ControlButton>
            </div>

            <div
                style={{
                    display: "flex",
                    justifyContent: "center",
                    marginTop: scalePx(10),
                    gap: scalePx(10),
                    flexWrap: "wrap",
                }}
            >
                <ControlButton
                    size="wide"
                    variant="primary"
                    onClick={() => window.location.href = "/rc"}
                >
                    摇杆驾驶
                </ControlButton>
                <ControlButton
                    size="wide"
                    variant="secondary"
                    onClick={() => window.location.href = "/wifi"}
                >
                    WiFi 配置
                </ControlButton>
                <ControlButton
                    size="wide"
                    variant="secondary"
                    onClick={() => window.location.href = "/arm-angles"}
                >
                    角度配置
                </ControlButton>
                <ControlButton
                    size="wide"
                    variant="secondary"
                    onClick={() => window.location.href = "/demo"}
                >
                    Demo
                </ControlButton>
                <ControlButton
                    size="wide"
                    variant="secondary"
                    onClick={() => window.location.href = "/gravity"}
                >
                    重力遥控
                </ControlButton>
                <ControlButton
                    size="wide"
                    variant="secondary"
                    onClick={() => redirect()}
                >
                    进入试验平台
                </ControlButton>
            </div>

            <div style={{marginTop: scalePx(20), opacity: 0.5, fontSize: scalePx(13)}}>
                <span style={{
                    width: "6px", height: "6px", borderRadius: "50%",
                    background: wsConnected ? "#22c55e" : "#ef4444",
                    display: "inline-block", marginRight: scalePx(6),
                }}/>
                {status}
            </div>
        </div>
    );
}

export default BaseControlPage;