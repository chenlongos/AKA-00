import {useEffect, useRef, useState} from "react";
import {api} from "../api";
import ControlButton from "../components/ControlButton.tsx";


const BaseControlPage = () => {
    const [ip, setIp] = useState("获取中...");
    const [status, setStatus] = useState("准备就绪");
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [cameraOn, setCameraOn] = useState(false);

    const currentActionRef = useRef<string | null>(null);

    // 摄像头状态
    useEffect(() => {
        api.camera.status().then(data => setCameraOn(data.camera_on)).catch(() => {});
    }, []);

    // 定时获取电机实时速度
    useEffect(() => {
        const fetchSpeed = async () => {
            try {
                const data = await api.motor.status();
                setLeftSpeed(data.left_speed ?? 0);
                setRightSpeed(data.right_speed ?? 0);
            } catch {}
        };

        fetchSpeed();
        const interval = setInterval(fetchSpeed, 500);
        return () => clearInterval(interval);
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
    }, []);

    const send = async (action: string) => {
        setStatus("执行: " + action);
        try {
            const text = await api.motor.action(action);
            if (text) {
                try {
                    console.log(JSON.parse(text));
                } catch {
                    console.log(text);
                }
            }
        } catch (err) {
            setStatus("错误: " + err);
        }
    };

    const handlePressStart = (action: string) => {
        currentActionRef.current = action;
        send(action);
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
            window.location.replace(`https://ai.maodouketang.cn/?ip=${encodeURIComponent(data.ip)}`);
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
                padding: "10px",
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
                    gap: "10px",
                    justifyItems: "center",
                    flexDirection: "column",
                    alignItems: "center",
                    position: "relative",
                }}
            >
                {/* 摄像头开关 */}
                <div style={{position: "absolute", top: "0", right: "0", display: "flex", alignItems: "center", gap: "6px"}}>
                    <span style={{fontSize: "11px", opacity: 0.6}}>摄像头</span>
                    <div
                        onClick={() => {
                            const req = cameraOn ? api.camera.close() : api.camera.open();
                            req.then(data => setCameraOn(data.camera_on)).catch(() => {});
                        }}
                        style={{
                            width: "44px",
                            height: "22px",
                            borderRadius: "11px",
                            background: cameraOn ? "#22c55e" : "#334155",
                            border: `1px solid ${cameraOn ? "#22c55e" : "#475569"}`,
                            cursor: "pointer",
                            position: "relative",
                            transition: "background 0.2s",
                        }}
                    >
                        <div style={{
                            position: "absolute",
                            top: "2px",
                            left: cameraOn ? "23px" : "2px",
                            width: "16px",
                            height: "16px",
                            borderRadius: "50%",
                            background: "white",
                            transition: "left 0.2s",
                        }}/>
                    </div>
                </div>

                <div/>
                <ControlButton
                    onPressStart={() => handlePressStart("up")}
                    onPressEnd={() => handlePressEnd()}
                >
                    前进
                </ControlButton>
                <div style={{display: 'flex', gap: "10px"}}>
                    <ControlButton
                        onPressStart={() => handlePressStart("left")}
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
                        onPressStart={() => handlePressStart("right")}
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
                    marginBottom: "20px",
                    padding: "10px",
                    background: "rgba(255,255,255,0.1)",
                    borderRadius: "8px",
                    display: "inline-flex",
                    gap: "30px",
                    fontSize: "14px",
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
                    gap: "10px",
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
                    marginTop: "10px",
                    gap: "10px",
                    flexWrap: "wrap",
                }}
            >
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
                    onClick={() => redirect()}
                >
                    进入试验平台
                </ControlButton>
            </div>

            <div style={{marginTop: "20px", opacity: 0.5, fontSize: "13px"}}>
                {status}
            </div>
        </div>
    );
}

export default BaseControlPage;