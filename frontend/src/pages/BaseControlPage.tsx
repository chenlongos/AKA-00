import {useEffect, useRef, useState} from "react";
import {sendAction} from "../api/socket.ts";
import ControlButton from "../components/ControlButton.tsx";

const FPS = 20
const frameInterval = 1000 / FPS

const BaseControlPage = () => {
    const [ip, setIp] = useState("获取中...");
    const [status, setStatus] = useState("准备就绪");
    const [isSimulator, setIsSimulator] = useState(false);

    // 当前正在执行的动作（用于模拟器每帧发送）
    const currentActionRef = useRef<string | null>(null);

    useEffect(() => {
        const getIp = () => {
            setStatus("获取 IP...");
            fetch("/api/ip")
                .then((res) => res.json())
                .then((data) => {
                    console.log("device ip:", data.ip);
                    setIp("IP: " + data.ip);
                    setStatus("准备就绪");
                })
                .catch(() => {
                    setStatus("获取 IP 失败");
                });
        };

        getIp();
    }, []);

    const send = (action: string) => {
        setStatus("执行: " + action);
        if (!isSimulator) {
            console.log("http send " + action);
            fetch(`/api/control?action=${action}&speed=50&time=0`)
                .then((res) => res.json())
                .then((data) => console.log(data))
                .catch((err) => setStatus("错误: " + err));
        }
    };

    // ==== 按钮事件处理 ====
    const handlePressStart = (action: string) => {
        currentActionRef.current = action;
        if (!isSimulator) {
            send(action); // 实车立即发
        }
    };

    const handlePressEnd = () => {
        currentActionRef.current = null;
        if (!isSimulator) {
            send("stop"); // 实车发 stop
        }
    };

    useEffect(() => {
        if (!isSimulator) return; // 只在模拟器模式运行
        let animationFrameId: number
        let lastTime = 0;
        const renderLoop = (currentTime: number) => {
            animationFrameId = window.requestAnimationFrame(renderLoop)
            const action = currentActionRef.current;
            const delta = currentTime - lastTime

            if (delta < frameInterval) return

            lastTime = currentTime - (delta % frameInterval)

            if (action !== null) {
                sendAction(action); // 每帧发送当前动作
            }
            animationFrameId = requestAnimationFrame(renderLoop);
        };

        animationFrameId = requestAnimationFrame(renderLoop);

        return () => {
            window.cancelAnimationFrame(animationFrameId)
        }

    }, [isSimulator])

    const redirect = () => {
        setStatus("获取 IP...");
        fetch("/api/ip")
            .then((res) => res.json())
            .then((data) => {
                const targetUrl = "https://ai.maodouketang.cn/";
                const fullUrl = `${targetUrl}?ip=${encodeURIComponent(data.ip)}`;
                window.location.replace(fullUrl);
            })
            .catch((err) => {
                console.error("跳转失败:", err);
                setStatus("跳转失败");
                alert("无法获取IP，请稍后重试");
            });
    };

    return (
        <div
            style={{
                fontFamily: "system-ui, sans-serif",
                background: "#0f172a",
                color: "white",
                minHeight: "100vh",
                padding: "10px",
                textAlign: "center",
                overflow: "hidden",
            }}
        >
            <h2>AKA-00 控制台</h2>
            <div style={{opacity: 0.6}}>{ip}</div>

            {/* 模式状态 */}
            <div
                style={{
                    marginTop: "15px",
                    padding: "8px 15px",
                    background: "#1e293b",
                    borderRadius: "12px",
                    display: "inline-block",
                    fontSize: "14px",
                }}
            >
                模式：
                <span
                    style={{
                        marginLeft: "8px",
                        color: isSimulator ? "#22c55e" : "#3b82f6",
                        fontWeight: "bold",
                    }}
                >
          {isSimulator ? "模拟" : "实车"}
        </span>
            </div>

            {/* 方向区 */}
            <div
                style={{
                    display: "flex",
                    gap: "20px",
                    justifyItems: "center",
                    flexDirection: "column",
                    alignItems: "center"
                }}
            >
                <div/>
                <ControlButton
                    onPressStart={() => handlePressStart("up")}
                    onPressEnd={() => handlePressEnd()}
                >
                    前进
                </ControlButton>
                <div style={{display: 'flex', gap: "20px"}}>
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

            {/* 功能按钮 */}
            <div
                style={{
                    display: "flex",
                    justifyContent: "center",
                    gap: "20px",
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
                    gap: "20px",
                    flexWrap: "wrap",
                }}
            >
                {/* 跳转 */}
                <div style={{marginTop: "40px"}}>
                    <ControlButton
                        size="wide"
                        variant="secondary"
                        onClick={() => redirect()}
                    >
                        进入试验平台
                    </ControlButton>
                </div>

                {/* 切换 */}
                <div style={{marginTop: "40px"}}>
                    <ControlButton
                        size="wide"
                        variant="secondary"
                        onClick={() => setIsSimulator(!isSimulator)}
                    >
                        切换模式
                    </ControlButton>
                </div>
            </div>

            <div style={{marginTop: "20px", opacity: 0.5, fontSize: "13px"}}>
                {status}
            </div>
        </div>
    );
}

export default BaseControlPage;