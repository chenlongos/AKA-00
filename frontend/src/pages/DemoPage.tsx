import {useState, useRef, useEffect} from "react";
import {api} from "../api";
import ControlButton from "../components/ControlButton.tsx";

interface Model {
    name: string;
    size: number;
    type: string;
}

const DEMO_SERVER_URL = "http://localhost:8888";

const DemoPage = () => {
    // Demo state
    const [demoStatus, setDemoStatus] = useState("准备就绪");
    const [output, setOutput] = useState<string>("");
    const [runningDemo, setRunningDemo] = useState<string | null>(null);
    const [conflictDemo, setConflictDemo] = useState(false);
    const [demoLoading, setDemoLoading] = useState(false);
    const runningDemoRef = useRef<string | null>(null);

    // Model download state
    const [models, setModels] = useState<Model[]>([]);
    const [modelListLoading, setModelListLoading] = useState(false);
    const [modelStatus, setModelStatus] = useState("准备就绪");
    const [downloading, setDownloading] = useState<string | null>(null);
    const [progress, setProgress] = useState<number>(0);

    useEffect(() => {
        fetchModels();
    }, []);

    // --- Demo handlers ---
    const runDemo = async (name: string) => {
        if (runningDemoRef.current !== null) {
            setDemoLoading(true);
            try {
                await api.demo.stop();
            } catch {}
            setDemoLoading(false);
            setDemoStatus(`${runningDemoRef.current} 已停止`);
            setRunningDemo(null);
            runningDemoRef.current = null;
            setConflictDemo(false);
            setOutput("");
            return;
        }

        setDemoStatus(`执行中: ${name}...`);
        setOutput("");
        setConflictDemo(false);
        setRunningDemo(name);
        runningDemoRef.current = name;

        try {
            const data = await api.demo.init(name);
            if (data.error) {
                if (data.pid) {
                    setDemoStatus("demo is already running");
                    setConflictDemo(true);
                } else {
                    setDemoStatus(`错误: ${data.error}`);
                    setRunningDemo(null);
                    runningDemoRef.current = null;
                }
                return;
            }

            setDemoLoading(true);
            setTimeout(() => setDemoLoading(false), 5000);

        } catch (err) {
            setDemoStatus(`错误: ${err}`);
            setRunningDemo(null);
            runningDemoRef.current = null;
        }
    };

    // --- Model download handlers ---
    const fetchModels = async () => {
        setModelListLoading(true);
        try {
            const res = await fetch(`${DEMO_SERVER_URL}/api/models`);
            const data = await res.json();
            setModels(data.models || []);
            setModelStatus(`共 ${data.models?.length || 0} 个模型`);
        } catch (err) {
            setModelStatus(`连接失败: ${err}`);
        } finally {
            setModelListLoading(false);
        }
    };

    const downloadModel = async (name: string) => {
        setDownloading(name);
        setProgress(0);
        setModelStatus(`下载中: ${name}...`);

        try {
            const res = await fetch("/api/demo/download_model_with_progress", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({
                    demo_name: "tennis",
                    model_name: name,
                    demo_server: DEMO_SERVER_URL,
                }),
            });
            const data = await res.json();

            if (data.error) {
                setModelStatus(`下载失败: ${data.error}`);
                setDownloading(null);
                return;
            }

            const taskId = data.task_id;
            const pollInterval = setInterval(async () => {
                try {
                    const progressRes = await fetch(`/api/demo/download_progress/${taskId}`);
                    const progressData = await progressRes.json();

                    if (progressData.status === "done") {
                        setProgress(100);
                        setModelStatus(`下载完成: ${name}`);
                        clearInterval(pollInterval);
                        setDownloading(null);
                    } else if (progressData.status === "error") {
                        setModelStatus(`下载失败: ${progressData.error}`);
                        clearInterval(pollInterval);
                        setDownloading(null);
                    } else {
                        setProgress(progressData.progress || 0);
                        setModelStatus(`下载中: ${name}... ${progressData.progress || 0}%`);
                    }
                } catch {
                    clearInterval(pollInterval);
                    setDownloading(null);
                }
            }, 200);

        } catch (err) {
            setModelStatus(`下载失败: ${err}`);
            setDownloading(null);
        }
    };

    const formatSize = (bytes: number) => {
        if (bytes < 1024) return `${bytes} B`;
        if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
        return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
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
                overflowY: "auto",
                overscrollBehavior: "contain",
            }}
        >
            <h2>Demo 控制台</h2>

            {/* ---- Demo 控制区 ---- */}
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
                    disabled={demoLoading}
                    loading={demoLoading}
                >
                    {runningDemo === "tennis" ? "停止 Tennis" : "Tennis Demo"}
                </ControlButton>
            </div>

            <div style={{marginTop: "10px", opacity: 0.5, fontSize: "13px"}}>
                {demoStatus}
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

            {/* ---- 分割线 ---- */}
            <hr
                style={{
                    maxWidth: "500px",
                    margin: "30px auto",
                    borderColor: "#334155",
                    borderStyle: "solid",
                }}
            />

            {/* ---- 模型下载区 ---- */}
            <h3 style={{marginTop: "0"}}>模型下载</h3>
            <div style={{fontSize: "13px", opacity: 0.7}}>
                从 Demo Server 获取模型到 demo/tennis
            </div>

            <div style={{marginTop: "16px"}}>
                <ControlButton
                    variant="secondary"
                    onClick={fetchModels}
                    disabled={modelListLoading}
                >
                    {modelListLoading ? "加载中..." : "刷新列表"}
                </ControlButton>
            </div>

            <div style={{marginTop: "12px", opacity: 0.5, fontSize: "13px"}}>
                {modelStatus}
                {downloading && progress > 0 && progress < 100 && (
                    <span> {progress}%</span>
                )}
            </div>

            {downloading && progress > 0 && (
                <div
                    style={{
                        width: "80%",
                        maxWidth: "400px",
                        height: "8px",
                        background: "#1e293b",
                        borderRadius: "4px",
                        margin: "10px auto",
                        overflow: "hidden",
                    }}
                >
                    <div
                        style={{
                            width: `${progress}%`,
                            height: "100%",
                            background: progress === 100 ? "#22c55e" : "#3b82f6",
                            transition: "width 0.2s ease",
                        }}
                    />
                </div>
            )}

            <div
                style={{
                    maxWidth: "500px",
                    margin: "20px auto",
                    textAlign: "left",
                }}
            >
                {models.length === 0 && !modelListLoading && (
                    <div style={{textAlign: "center", opacity: 0.5, padding: "40px"}}>
                        暂无模型
                    </div>
                )}

                {models.map((model) => (
                    <div
                        key={model.name}
                        style={{
                            display: "flex",
                            justifyContent: "space-between",
                            alignItems: "center",
                            padding: "12px 16px",
                            background: "#1e293b",
                            borderRadius: "8px",
                            marginBottom: "8px",
                        }}
                    >
                        <div>
                            <div style={{fontWeight: 500}}>{model.name}</div>
                            <div style={{fontSize: "12px", opacity: 0.6, marginTop: "4px"}}>
                                {formatSize(model.size)} • {model.type.toUpperCase()}
                            </div>
                        </div>
                        <ControlButton
                            variant="success"
                            size="wide"
                            onClick={() => downloadModel(model.name)}
                            disabled={downloading !== null}
                        >
                            {downloading === model.name ? "下载中..." : "下载"}
                        </ControlButton>
                    </div>
                ))}
            </div>

            {/* ---- 返回 ---- */}
            <div style={{marginTop: "30px", marginBottom: "20px"}}>
                <ControlButton
                    variant="secondary"
                    onClick={() => window.location.href = "/"}
                >
                    返回
                </ControlButton>
            </div>
        </div>
    );
};

export default DemoPage;