import {useState, useRef, useEffect, useCallback} from "react";
import {api} from "../api";
import ControlButton from "../components/ControlButton.tsx";
import {useViewportScale} from "../hooks/useViewportScale";

interface Model {
    name: string;
    file: string;
    size: number;
    type: string;
}

const DEMO_SERVER_URL = import.meta.env.VITE_DEMO_SERVER_URL || "http://localhost:8888";

const DemoPage = () => {
    const {scalePx} = useViewportScale();
    // 本地 Demo 列表
    const [demos, setDemos] = useState<string[]>([]);
    const [demoStatus, setDemoStatus] = useState("准备就绪");
    const [runningDemo, setRunningDemo] = useState<string | null>(null);
    const [demoLoading, setDemoLoading] = useState(false);
    const runningDemoRef = useRef<string | null>(null);

    // 远端模型列表
    const [models, setModels] = useState<Model[]>([]);
    const [listLoading, setListLoading] = useState(false);
    const [listStatus, setListStatus] = useState("准备就绪");
    const [downloading, setDownloading] = useState<string | null>(null);
    const [progress, setProgress] = useState<number>(0);

    const fetchDemoList = useCallback(() => {
        api.demo.list().then(data => {
            setDemos(data.demos || []);
        }).catch(() => {});
    }, []);

    const fetchModels = useCallback(async () => {
        setListLoading(true);
        try {
            const res = await fetch(`${DEMO_SERVER_URL}/api/models`);
            const data = await res.json();
            setModels(data.models || []);
            setListStatus(`共 ${data.models?.length || 0} 个模型`);
        } catch (err) {
            setListStatus(`连接 Demo Server 失败: ${err}`);
        } finally {
            setListLoading(false);
        }
    }, []);

    useEffect(() => {
        fetchDemoList();
        fetchModels();
    }, [fetchDemoList, fetchModels]);

    // --- 本地 Demo 操作 ---
    const runDemo = async (name: string) => {
        // 如果已有运行中的 demo，先停止
        if (runningDemoRef.current !== null) {
            setDemoLoading(true);
            try { await api.demo.stop(); } catch {}
            setDemoLoading(false);
            setDemoStatus(`${runningDemoRef.current} 已停止`);
            setRunningDemo(null);
            runningDemoRef.current = null;
            // 如果点的是正在运行的 demo，只停止不启动
            if (runningDemoRef.current === name || runningDemo === name) {
                return;
            }
        }

        setDemoStatus(`执行中: ${name}...`);
        setRunningDemo(name);
        runningDemoRef.current = name;

        try {
            const data = await api.demo.init(name);
            if (data.error) {
                if (data.pid) {
                    setDemoStatus("demo is already running");
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

    // --- 远端模型下载 ---
    const downloadModel = async (name: string) => {
        setDownloading(name);
        setProgress(0);
        setListStatus(`下载中: ${name}...`);

        try {
            const res = await fetch("/api/demo/download_model_with_progress", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({
                    model_name: name,
                    demo_server: DEMO_SERVER_URL,
                }),
            });
            const data = await res.json();

            if (data.error) {
                setListStatus(`下载失败: ${data.error}`);
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
                        setListStatus(`下载完成: ${name}`);
                        clearInterval(pollInterval);
                        setDownloading(null);
                        // 刷新 demo 名称（目录可能已被重命名）
                        fetchDemoList();
                    } else if (progressData.status === "error") {
                        setListStatus(`下载失败: ${progressData.error}`);
                        clearInterval(pollInterval);
                        setDownloading(null);
                    } else {
                        setProgress(progressData.progress || 0);
                        setListStatus(`下载中: ${name}... ${progressData.progress || 0}%`);
                    }
                } catch {
                    clearInterval(pollInterval);
                    setDownloading(null);
                }
            }, 200);

        } catch (err) {
            setListStatus(`下载失败: ${err}`);
            setDownloading(null);
        }
    };

    const formatSize = (bytes: number) => {
        if (bytes < 1024) return `${bytes} B`;
        if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
        return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
    };

    const displayName = (name: string) => name.charAt(0).toUpperCase() + name.slice(1);

    return (
        <div
            style={{
                fontFamily: "system-ui, sans-serif",
                background: "#0f172a",
                color: "white",
                height: "100dvh",
                padding: scalePx(10),
                textAlign: "center",
                overflowY: "auto",
                overscrollBehavior: "contain",
            }}
        >
            <h2>Demo 控制台</h2>

            {/* ---- 本地 Demo 列表 ---- */}
            <div style={{marginTop: scalePx(20)}}>
                {demos.length === 0 ? (
                    <div style={{opacity: 0.5}}>未找到本地 Demo</div>
                ) : (
                    <div style={{display: "flex", flexDirection: "column", alignItems: "center", gap: scalePx(10)}}>
                        {demos.map(name => (
                            <ControlButton
                                key={name}
                                size="wide"
                                variant={runningDemo === name ? "danger" : "success"}
                                onClick={() => runDemo(name)}
                                disabled={demoLoading && runningDemo !== name}
                                loading={demoLoading && runningDemo === name}
                            >
                                {runningDemo === name ? `停止 ${displayName(name)}` : `${displayName(name)} Demo`}
                            </ControlButton>
                        ))}
                    </div>
                )}
            </div>

            <div style={{marginTop: scalePx(10), opacity: 0.5, fontSize: scalePx(13)}}>
                {demoStatus}
            </div>

            {/* ---- 分割线 ---- */}
            <hr
                style={{
                    maxWidth: scalePx(500),
                    margin: "30px auto",
                    borderColor: "#334155",
                    borderStyle: "solid",
                }}
            />

            {/* ---- 远端模型下载 ---- */}
            <h3 style={{marginTop: "0"}}>模型下载</h3>
            <div style={{fontSize: scalePx(13), opacity: 0.7}}>
                下载模型将替换本地 Demo 行为
            </div>

            <div style={{marginTop: scalePx(16)}}>
                <ControlButton
                    variant="secondary"
                    onClick={fetchModels}
                    disabled={listLoading}
                >
                    {listLoading ? "加载中..." : "刷新列表"}
                </ControlButton>
            </div>

            <div style={{marginTop: scalePx(12), opacity: 0.5, fontSize: scalePx(13)}}>
                {listStatus}
                {downloading && progress > 0 && progress < 100 && (
                    <span> {progress}%</span>
                )}
            </div>

            {downloading && progress > 0 && (
                <div
                    style={{
                        width: "80%",
                        maxWidth: scalePx(400),
                        height: scalePx(8),
                        background: "#1e293b",
                        borderRadius: scalePx(4),
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
                    maxWidth: scalePx(500),
                    margin: "20px auto",
                    textAlign: "left",
                }}
            >
                {models.length === 0 && !listLoading && (
                    <div style={{textAlign: "center", opacity: 0.5, padding: scalePx(40)}}>
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
                            padding: `${scalePx(12)} ${scalePx(16)}`,
                            background: "#1e293b",
                            borderRadius: scalePx(8),
                            marginBottom: scalePx(8),
                        }}
                    >
                        <div>
                            <div style={{fontWeight: 500}}>{model.name}</div>
                            <div style={{fontSize: scalePx(12), opacity: 0.6, marginTop: scalePx(4)}}>
                                {model.file} • {formatSize(model.size)} • {model.type.toUpperCase()}
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
            <div style={{marginTop: scalePx(30), paddingBottom: scalePx(40)}}>
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
