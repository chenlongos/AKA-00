import {useEffect, useState} from "react";
import {sendAction} from "../api/socket.ts";
import ControlButton from "../components/ControlButton.tsx";

const BaseControlPage = () => {
    const [ip, setIp] = useState("获取中...");
    const [status, setStatus] = useState("准备就绪");
    const [isSimulator, setIsSimulator] = useState(false);

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
            fetch(`/api/control?action=${action}&speed=50&time=0`)
                .then((res) => res.json())
                .then((data) => console.log(data))
                .catch((err) => setStatus("错误: " + err));
        } else {
            sendAction(action)
        }
    };

    useEffect(() => {
        let animationFrameId: number
        const renderLoop = () => {
            animationFrameId = window.requestAnimationFrame(renderLoop)
        }

        renderLoop()

        return () => {
            window.cancelAnimationFrame(animationFrameId)
        }

    }, [send])

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
                    onPressStart={() => send("up")}
                    onPressEnd={() => send("stop")}
                >
                    前进
                </ControlButton>
                <div style={{display: 'flex', gap: "20px"}}>
                    <ControlButton
                        onPressStart={() => send("left")}
                        onPressEnd={() => send("stop")}
                    >
                        左转
                    </ControlButton>
                    <ControlButton
                        variant="danger"
                        onPressStart={() => send("stop")}
                    >
                        停止
                    </ControlButton>
                    <ControlButton
                        onPressStart={() => send("right")}
                        onPressEnd={() => send("stop")}
                    >
                        右转
                    </ControlButton>
                </div>

                <ControlButton
                    onPressStart={() => send("down")}
                    onPressEnd={() => send("stop")}
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