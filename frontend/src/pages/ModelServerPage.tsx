import {useState, useEffect} from "react";
import ControlButton from "../components/ControlButton.tsx";

interface Model {
    name: string;
    size: number;
    type: string;
}

const DEMO_SERVER_URL = "http://localhost:8888";

const ModelServerPage = () => {
    const [models, setModels] = useState<Model[]>([]);
    const [loading, setLoading] = useState(false);
    const [status, setStatus] = useState("准备就绪");
    const [downloading, setDownloading] = useState<string | null>(null);
    const [progress, setProgress] = useState<number>(0);

    useEffect(() => {
        fetchModels();
    }, []);

    const fetchModels = async () => {
        setLoading(true);
        try {
            const res = await fetch(`${DEMO_SERVER_URL}/api/models`);
            const data = await res.json();
            setModels(data.models || []);
            setStatus(`共 ${data.models?.length || 0} 个模型`);
        } catch (err) {
            setStatus(`连接失败: ${err}`);
        } finally {
            setLoading(false);
        }
    };

    const downloadModel = async (name: string) => {
        setDownloading(name);
        setProgress(0);
        setStatus(`下载中: ${name}...`);

        try {
            // 启动下载
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
                setStatus(`下载失败: ${data.error}`);
                setDownloading(null);
                return;
            }

            // 轮询进度
            const taskId = data.task_id;
            const pollInterval = setInterval(async () => {
                try {
                    const progressRes = await fetch(`/api/demo/download_progress/${taskId}`);
                    const progressData = await progressRes.json();

                    if (progressData.status === "done") {
                        setProgress(100);
                        setStatus(`下载完成: ${name}`);
                        clearInterval(pollInterval);
                        setDownloading(null);
                    } else if (progressData.status === "error") {
                        setStatus(`下载失败: ${progressData.error}`);
                        clearInterval(pollInterval);
                        setDownloading(null);
                    } else {
                        setProgress(progressData.progress || 0);
                        setStatus(`下载中: ${name}... ${progressData.progress || 0}%`);
                    }
                } catch {
                    clearInterval(pollInterval);
                    setDownloading(null);
                }
            }, 200);

        } catch (err) {
            setStatus(`下载失败: ${err}`);
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
            }}
        >
            <h2>模型下载</h2>
            <div style={{marginTop: "10px", fontSize: "13px", opacity: 0.7}}>
                从 Demo Server 获取模型到 demo/tennis
            </div>

            <div style={{marginTop: "20px"}}>
                <ControlButton
                    variant="secondary"
                    onClick={fetchModels}
                    disabled={loading}
                >
                    {loading ? "加载中..." : "刷新列表"}
                </ControlButton>
            </div>

            <div style={{marginTop: "20px", opacity: 0.5, fontSize: "13px"}}>
                {status}
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
                {models.length === 0 && !loading && (
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

            <div style={{marginTop: "30px"}}>
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

export default ModelServerPage;