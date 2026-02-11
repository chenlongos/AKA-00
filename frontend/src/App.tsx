import {useEffect, useState} from "react";

function App() {
    const [ip, setIp] = useState("获取中...");
    const [status, setStatus] = useState("准备就绪");

    useEffect(() => {
        const getIp = () => {
            setStatus("获取 IP...");
            fetch("/ip")
                .then((res) => res.json())
                .then((data) => {
                    console.log("device ip:", data.ip);
                    setIp("IP: " + data.ip);
                    setStatus("准备就绪");
                })
                .catch((err) => {
                    console.error(err);
                    setStatus("获取 IP 失败");
                });
        };

        getIp();
    }, []);

    const send = (action: string) => {
        setStatus("执行: " + action);
        fetch(`/control?action=${action}&speed=50&time=0`)
            .then((res) => res.json())
            .then((data) => console.log(data))
            .catch((err) => setStatus("错误: " + err));
    };

    const redirect = () => {
        setStatus("获取 IP...");
        fetch("/ip")
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
        <div style={{
            fontFamily: "sans-serif",
            textAlign: "center",
            background: "#f0f0f0",
            minHeight: "100vh",
            padding: "20px"
        }}>
            <h2>AKA-00机器人控制</h2>
            <h3>{ip}</h3>

            <div style={{
                display: "grid",
                gridTemplateColumns: "repeat(3, 100px)",
                gridGap: "15px",
                justifyContent: "center",
                marginTop: "50px"
            }}>
                <div style={{gridColumn: 2}}>
                    <button style={{
                        width: "90px",
                        height: "90px",
                        border: "none",
                        borderRadius: "15px",
                        background: "#2196F3",
                        color: "white",
                        fontWeight: "bold",
                        fontSize: "16px",
                        boxShadow: "0 4px #1976D2",
                        cursor: "pointer"
                    }} onClick={() => send("up")}>前进
                    </button>
                </div>
                <div style={{gridRow: 2, gridColumn: 1}}>
                    <button style={{
                        width: "90px",
                        height: "90px",
                        border: "none",
                        borderRadius: "15px",
                        background: "#2196F3",
                        color: "white",
                        fontWeight: "bold",
                        fontSize: "16px",
                        boxShadow: "0 4px #1976D2",
                        cursor: "pointer"
                    }} onClick={() => send("left")}>左转
                    </button>
                </div>
                <div style={{gridRow: 2, gridColumn: 2}}>
                    <button style={{
                        background: "#f44336",
                        boxShadow: "0 4px #d32f2f",
                        width: "90px",
                        height: "90px",
                        borderRadius: "15px",
                        color: "#fff",
                        fontWeight: "bold",
                        fontSize: "16px",
                        border: "none"
                    }} onClick={() => send("stop")}>
                        停止
                    </button>
                </div>
                <div style={{gridRow: 2, gridColumn: 3}}>
                    <button style={{
                        width: "90px",
                        height: "90px",
                        border: "none",
                        borderRadius: "15px",
                        background: "#2196F3",
                        color: "white",
                        fontWeight: "bold",
                        fontSize: "16px",
                        boxShadow: "0 4px #1976D2",
                        cursor: "pointer"
                    }} onClick={() => send("right")}>右转
                    </button>
                </div>
                <div style={{gridRow: 3, gridColumn: 2}}>
                    <button style={{
                        width: "90px",
                        height: "90px",
                        border: "none",
                        borderRadius: "15px",
                        background: "#2196F3",
                        color: "white",
                        fontWeight: "bold",
                        fontSize: "16px",
                        boxShadow: "0 4px #1976D2",
                        cursor: "pointer"
                    }} onClick={() => send("down")}>后退
                    </button>
                </div>
            </div>

            <div style={{marginTop: "40px", display: "flex", justifyContent: "center", gap: "20px"}}>
                <button style={{
                    background: "#ff9800",
                    boxShadow: "0 4px #ef6c00",
                    width: "120px",
                    height: "90px",
                    borderRadius: "15px",
                    color: "#fff",
                    fontWeight: "bold",
                    fontSize: "16px",
                    border: "none"
                }} onClick={() => send("grab")}>
                    抓取 (Grab)
                </button>
                <button style={{
                    background: "#4CAF50",
                    boxShadow: "0 4px #388E3C",
                    width: "120px",
                    height: "90px",
                    borderRadius: "15px",
                    color: "#fff",
                    fontWeight: "bold",
                    fontSize: "16px",
                    border: "none"
                }} onClick={() => send("release")}>
                    释放 (Release)
                </button>
            </div>

            <div style={{marginTop: "20px", color: "#666"}}>{status}</div>

            <div style={{marginTop: "20px"}}>
                <button style={{
                    background: "#9C27B0",
                    boxShadow: "0 4px #7B1FA2",
                    width: "140px",
                    height: "100px",
                    fontSize: "14px",
                    border: "none",
                    borderRadius: "15px",
                    color: "#fff"
                }} onClick={redirect}>
                    点击进入实训平台
                </button>
            </div>
        </div>
    );
}

export default App;
