import {useEffect, useRef, useState} from "react";
import ControlButton from "../components/ControlButton.tsx";


const BaseControlPage = () => {
    const [ip, setIp] = useState("获取中...");
    const [status, setStatus] = useState("准备就绪");

    // 当前正在执行的动作（用于模拟器每帧发送）
    const currentActionRef = useRef<string | null>(null);

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
                minHeight: "100vh",
                padding: "10px",
                textAlign: "center",
                overflow: "hidden",
            }}
        >
            <h2>AKA-00 控制台</h2>
            <div style={{opacity: 0.6}}>{ip}</div>

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
                    marginTop: "20px",
                    gap: "20px",
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
