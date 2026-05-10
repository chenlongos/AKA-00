import {useEffect, useRef, useState} from "react";
import ControlButton from "../components/ControlButton.tsx";


const BaseControlPage = () => {
    const [ip, setIp] = useState("获取中...");
    const [status, setStatus] = useState("准备就绪");
    const [leftSpeed, setLeftSpeed] = useState(0);
    const [rightSpeed, setRightSpeed] = useState(0);
    const [cameraOn, setCameraOn] = useState(false);

    // 当前正在执行的动作（用于模拟器每帧发送）
    const currentActionRef = useRef<string | null>(null);

    // 摄像头状态
    useEffect(() => {
        fetch("/api/camera/status")
            .then(res => res.json())
            .then(data => setCameraOn(data.camera_on))
            .catch(() => {});
    }, []);

    // 定时获取电机实时速度
    useEffect(() => {
        const fetchSpeed = async () => {
            try {
                const timestamp = Date.now();
                const res = await fetch(`/api/motor_status?timestamp=${timestamp}`);
                if (res.ok) {
                    const data = await res.json();
                    setLeftSpeed(data.left_speed ?? 0);
                    setRightSpeed(data.right_speed ?? 0);
                }
            } catch {
                // 忽略错误，避免刷屏
            }
        };

        fetchSpeed();
        const interval = setInterval(fetchSpeed, 100);
        return () => clearInterval(interval);
    }, []);

    // URL哈希命令监听
    useEffect(() => {
        const processHash = () => {
            const hash = window.location.hash;
            if (hash) {
                fetch(`/api/raw_command?cmd=${encodeURIComponent(hash)}`).then(() => {
                    // 命令发送后清空哈希，避免重复执行
                    window.history.replaceState(null, document.title, window.location.pathname);
                }).catch(console.error);
            }
        };

        // 初始检查
        processHash();
        
        // 监听哈希变化
        window.addEventListener('hashchange', processHash);
        
        return () => window.removeEventListener('hashchange', processHash);
    }, []);

    useEffect(() => {
        const getIp = async () => {
            setStatus("获取 IP...");
            try {
                const res = await fetch("/ip");
                if (!res.ok) throw new Error("请求失败");
                const data = await res.json();
                console.log("device ip:", data.ip);
                setIp("IP: " + data.ip);
                setStatus("准备就绪");
            } catch {
                setStatus("获取 IP 失败");
            }
        };

        getIp();

        // 切回控制台时重新加载 PWM 配置以恢复状态
        fetch("/api/base_pwm_channels")
            .then(res => res.json())
            .then(data => {
                // 用现有配置 POST 回去，触发 PWM 重启
                return fetch("/api/base_pwm_channels", {
                    method: "POST",
                    headers: {"Content-Type": "application/json"},
                    body: JSON.stringify({pwm_channels: data.pwm_channels}),
                });
            })
            .catch(() => {});
    }, []);

    const send = async (action: string) => {
        setStatus("执行: " + action);
        console.log("http send " + action);
        try {
            const res = await fetch(`/api/control?action=${action}&speed=50&time=0`);
            if (!res.ok) throw new Error("请求失败");
            const text = await res.text();
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

    // ==== 按钮事件处理 ====
    const handlePressStart = (action: string) => {
        currentActionRef.current = action;
        send(action); // 实车立即发
    };

    const handlePressEnd = () => {
        currentActionRef.current = null;
        send("stop"); // 实车发 stop
    };

    const redirect = async () => {
        setStatus("获取 IP...");
        try {
            const res = await fetch("/ip");
            if (!res.ok) throw new Error("请求失败");
            const data = await res.json();
            if (!data?.ip) throw new Error("无IP数据");
            const targetUrl = "https://ai.maodouketang.cn/";
            const fullUrl = `${targetUrl}?ip=${encodeURIComponent(data.ip)}`;
            window.location.replace(fullUrl);
        } catch (err) {
            console.error("跳转失败:", err);
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
                {/* 摄像头开关 - 区域内右上角 */}
                <div style={{position: "absolute", top: "0", right: "0", display: "flex", alignItems: "center", gap: "6px"}}>
                    <span style={{fontSize: "11px", opacity: 0.6}}>摄像头</span>
                    <div
                        onClick={() => {
                            const action = cameraOn ? "close" : "open";
                            fetch("/api/camera", {
                                method: "POST",
                                headers: {"Content-Type": "application/json"},
                                body: JSON.stringify({action}),
                            })
                                .then(res => res.json())
                                .then(data => setCameraOn(data.camera_on))
                                .catch(() => {});
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
                {/* 跳转 */}
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
