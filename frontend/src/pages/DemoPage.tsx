import {useState} from "react";
import ControlButton from "../components/ControlButton.tsx";

const DemoPage = () => {
    const [status, setStatus] = useState("准备就绪");
    const [output, setOutput] = useState<string>("");

    const runDemo = async (name: string) => {
        setStatus(`执行中: ${name}...`);
        setOutput("");
        try {
            const res = await fetch("/api/demo/init", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({name}),
            });
            const data = await res.json();
            if (res.ok) {
                setStatus(`${name} 执行完成 (返回码: ${data.returncode})`);
                const lines = [];
                if (data.stdout) lines.push(`stdout:\n${data.stdout}`);
                if (data.stderr) lines.push(`stderr:\n${data.stderr}`);
                setOutput(lines.join("\n\n") || "无输出");
                console.log(data);
            } else {
                setStatus(`错误: ${data.error}`);
            }
        } catch (err) {
            setStatus(`错误: ${err}`);
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
            }}
        >
            <h2>Demo 控制台</h2>

            <div
                style={{
                    display: "flex",
                    justifyContent: "center",
                    gap: "20px",
                    flexWrap: "wrap",
                    marginTop: "20px",
                }}
            >
                <ControlButton
                    size="wide"
                    variant="success"
                    onClick={() => runDemo("tennis")}
                >
                    Tennis Demo
                </ControlButton>
            </div>

            <div style={{marginTop: "20px", opacity: 0.5, fontSize: "13px"}}>
                {status}
            </div>

            <div style={{marginTop: "20px"}}>
                <ControlButton
                    variant="secondary"
                    onClick={() => window.location.href = "/"}
                >
                    返回
                </ControlButton>
            </div>

            {output && (
                <div
                    style={{
                        marginTop: "20px",
                        padding: "10px",
                        background: "#1e293b",
                        borderRadius: "8px",
                        textAlign: "left",
                        fontFamily: "monospace",
                        fontSize: "12px",
                        whiteSpace: "pre-wrap",
                        maxWidth: "600px",
                        margin: "20px auto",
                    }}
                >
                    {output}
                </div>
            )}
        </div>
    );
};

export default DemoPage;