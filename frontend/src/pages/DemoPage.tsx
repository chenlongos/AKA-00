import {useState, useRef} from "react";
import ControlButton from "../components/ControlButton.tsx";

const DemoPage = () => {
    const [status, setStatus] = useState("准备就绪");
    const [output, setOutput] = useState<string>("");
    const [runningDemo, setRunningDemo] = useState<string | null>(null);
    const [conflictDemo, setConflictDemo] = useState(false);
    const [loading, setLoading] = useState(false);
    const runningDemoRef = useRef<string | null>(null);

    const runDemo = async (name: string) => {
        if (runningDemoRef.current !== null) {
            // stop current demo
            setLoading(true);
            try {
                await fetch("/api/demo/stop", {
                    method: "POST",
                    headers: {"Content-Type": "application/json"},
                    body: JSON.stringify({name: runningDemoRef.current}),
                });
            } catch {}
            setLoading(false);
            setStatus(`${runningDemoRef.current} 已停止`);
            setRunningDemo(null);
            runningDemoRef.current = null;
            setConflictDemo(false);
            setOutput("");
            return;
        }

        setStatus(`执行中: ${name}...`);
        setOutput("");
        setConflictDemo(false);
        setRunningDemo(name);
        runningDemoRef.current = name;

        try {
            const res = await fetch("/api/demo/init", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({name}),
            });
            if (!res.ok) {
                const data = await res.json();
                if (res.status === 409) {
                    setStatus(`demo is already running`);
                    setConflictDemo(true);
                } else {
                    setStatus(`错误: ${data.error}`);
                    setRunningDemo(null);
                    runningDemoRef.current = null;
                }
                return;
            }

            setLoading(true);
            setTimeout(() => setLoading(false), 5000);

        } catch (err) {
            setStatus(`错误: ${err}`);
            setRunningDemo(null);
            runningDemoRef.current = null;
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
                    variant={runningDemo === "tennis" || conflictDemo ? "danger" : "success"}
                    onClick={() => runDemo("tennis")}
                    disabled={loading}
                    loading={loading}
                >
                    {runningDemo === "tennis" ? "停止 Tennis" : "Tennis Demo"}
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