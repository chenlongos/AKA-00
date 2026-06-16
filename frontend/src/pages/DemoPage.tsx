import {useState, useRef, useEffect, useCallback} from "react";
import {api} from "../api";
import ControlButton from "../components/ControlButton.tsx";
import Page from "../components/Page";
import Card from "../components/Card";
import {S} from "../styles";
import {useViewportScale} from "../hooks/useViewportScale";

interface Model { name: string; file: string; size: number; type: string; }

const DEMO_SERVER_URL = import.meta.env.VITE_DEMO_SERVER_URL || "http://localhost:8888";

const DemoPage = () => {
    const {scalePx} = useViewportScale();
    const [demos, setDemos] = useState<string[]>([]);
    const [demoStatus, setDemoStatus] = useState("准备就绪");
    const [runningDemo, setRunningDemo] = useState<string | null>(null);
    const [demoLoading, setDemoLoading] = useState(false);
    const runningDemoRef = useRef<string | null>(null);

    const [models, setModels] = useState<Model[]>([]);
    const [listLoading, setListLoading] = useState(false);
    const [listStatus, setListStatus] = useState("准备就绪");
    const [downloading, setDownloading] = useState<string | null>(null);
    const [progress, setProgress] = useState<number>(0);

    const fetchDemoList = useCallback(() => { api.demo.list().then(data => setDemos(data.demos || [])).catch(() => {}); }, []);
    const fetchModels = useCallback(async () => {
        setListLoading(true);
        try {
            const res = await fetch(`${DEMO_SERVER_URL}/api/models`);
            const data = await res.json();
            setModels(data.models || []);
            setListStatus(`共 ${data.models?.length || 0} 个模型`);
        } catch (err) { setListStatus(`连接 Demo Server 失败: ${err}`); }
        finally { setListLoading(false); }
    }, []);

    useEffect(() => { fetchDemoList(); fetchModels(); }, [fetchDemoList, fetchModels]);

    const runDemo = async (name: string) => {
        if (runningDemoRef.current !== null) {
            setDemoLoading(true);
            try { await api.demo.stop(); } catch {}
            setDemoLoading(false);
            setDemoStatus(`${runningDemoRef.current} 已停止`);
            setRunningDemo(null);
            runningDemoRef.current = null;
            if (runningDemoRef.current === name || runningDemo === name) return;
        }
        setDemoStatus(`执行中: ${name}...`);
        setRunningDemo(name);
        runningDemoRef.current = name;
        try {
            const data = await api.demo.init(name);
            if (data.error) {
                setDemoStatus(data.pid ? "demo is already running" : `错误: ${data.error}`);
                if (!data.pid) { setRunningDemo(null); runningDemoRef.current = null; }
                return;
            }
            setDemoLoading(true);
            setTimeout(() => setDemoLoading(false), 5000);
        } catch (err) { setDemoStatus(`错误: ${err}`); setRunningDemo(null); runningDemoRef.current = null; }
    };

    const downloadModel = async (name: string) => {
        setDownloading(name); setProgress(0); setListStatus(`下载中: ${name}...`);
        try {
            const res = await fetch("/api/demo/download_model_with_progress", {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify({model_name: name, demo_server: DEMO_SERVER_URL})});
            const data = await res.json();
            if (data.error) { setListStatus(`下载失败: ${data.error}`); setDownloading(null); return; }
            const taskId = data.task_id;
            const poll = setInterval(async () => {
                try {
                    const pr = await fetch(`/api/demo/download_progress/${taskId}`);
                    const pd = await pr.json();
                    if (pd.status === "done") { setProgress(100); setListStatus(`下载完成: ${name}`); clearInterval(poll); setDownloading(null); fetchDemoList(); }
                    else if (pd.status === "error") { setListStatus(`下载失败: ${pd.error}`); clearInterval(poll); setDownloading(null); }
                    else { setProgress(pd.progress || 0); }
                } catch { clearInterval(poll); setDownloading(null); }
            }, 200);
        } catch (err) { setListStatus(`下载失败: ${err}`); setDownloading(null); }
    };

    const formatSize = (bytes: number) => bytes < 1024 ? `${bytes} B` : bytes < 1024 * 1024 ? `${(bytes / 1024).toFixed(1)} KB` : `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
    const displayName = (name: string) => name.charAt(0).toUpperCase() + name.slice(1);

    const maxW = {width: "100%", maxWidth: scalePx(420)};

    return (
        <Page center>
            <h2 style={{fontSize: scalePx(17), fontWeight: 700, marginBottom: scalePx(2), marginTop: "20px"}}>Demo 控制台</h2>

            {/* ====== 本地 Demo ====== */}
            <div style={{...maxW, marginTop: scalePx(14)}}>
                <div style={{...S.rowBetween, marginBottom: scalePx(8)}}>
                    <h3 style={{fontSize: scalePx(14), fontWeight: 600, margin: 0}}>本地 Demo</h3>
                    <span style={{fontSize: scalePx(11), color: "var(--color-text-dim)"}}>
                        {runningDemo ? `运行中: ${displayName(runningDemo)}` : `${demos.length} 个可用`}
                    </span>
                </div>

                {demos.length === 0 ? (
                    <Card>
                        <div style={{textAlign: "center", padding: scalePx(24), color: "var(--color-text-muted)", fontSize: scalePx(13)}}>
                            未找到本地 Demo
                        </div>
                    </Card>
                ) : (
                    <div style={{display: "flex", flexDirection: "column", gap: scalePx(8)}}>
                        {demos.map(name => {
                            const isRunning = runningDemo === name;
                            return (
                                <Card key={name} marginBottom={0} style={{
                                    border: isRunning ? "2px solid var(--color-success)" : "2px solid transparent",
                                    transition: "border-color 0.2s",
                                }}>
                                    <div style={{display: "flex", alignItems: "center", justifyContent: "space-between"}}>
                                        <div style={{display: "flex", alignItems: "center", gap: scalePx(10)}}>
                                            <span style={{
                                                width: scalePx(8), height: scalePx(8), borderRadius: "50%",
                                                background: isRunning ? "var(--color-success)" : "var(--color-bg-elevated)",
                                                boxShadow: isRunning ? "0 0 8px rgba(34,197,94,0.5)" : "none",
                                                transition: "all 0.3s",
                                                flexShrink: 0,
                                            }} />
                                            <div>
                                                <div style={{fontWeight: 600, fontSize: scalePx(14), textTransform: "capitalize"}}>{name}</div>
                                                <div style={{fontSize: scalePx(11), color: "var(--color-text-dim)", marginTop: 1}}>
                                                    {isRunning ? "● 运行中" : "○ 已停止"}
                                                </div>
                                            </div>
                                        </div>
                                        <ControlButton
                                            variant={isRunning ? "danger" : "success"}
                                            size="small"
                                            onClick={() => runDemo(name)}
                                            disabled={demoLoading && !isRunning}
                                            loading={demoLoading && isRunning}
                                        >
                                            {isRunning ? "停止" : "启动"}
                                        </ControlButton>
                                    </div>
                                </Card>
                            );
                        })}
                    </div>
                )}
            </div>

            {/* 本地状态 */}
            {demoStatus && demoStatus !== "准备就绪" && (
                <div style={{
                    ...maxW, marginTop: scalePx(6), textAlign: "center",
                    fontSize: scalePx(11), color: demoStatus.includes("错误") ? "var(--color-danger)" : "var(--color-text-dim)",
                }}>
                    {demoStatus}
                </div>
            )}

            {/* ====== 分割 ====== */}
            <div style={{
                ...maxW, marginTop: scalePx(20), marginBottom: scalePx(20),
                height: 1, background: "var(--color-border-light)",
                position: "relative",
            }}>
                <span style={{
                    position: "absolute", left: "50%", top: "50%",
                    transform: "translate(-50%, -50%)",
                    background: "var(--color-bg)", padding: `0 ${scalePx(12)}`,
                    fontSize: scalePx(10), color: "var(--color-text-dim)",
                    textTransform: "uppercase", letterSpacing: "2px",
                }}>
                    模型商店
                </span>
            </div>

            {/* ====== 模型下载 ====== */}
            <div style={maxW}>
                <div style={{...S.rowBetween, marginBottom: scalePx(8)}}>
                    <h3 style={{fontSize: scalePx(14), fontWeight: 600, margin: 0}}>远端模型</h3>
                    <ControlButton variant="secondary" size="small" onClick={fetchModels} disabled={listLoading}>
                        {listLoading ? "加载中..." : "🔄 刷新"}
                    </ControlButton>
                </div>

                <div style={{fontSize: scalePx(11), color: "var(--color-text-dim)", marginBottom: scalePx(10)}}>
                    {listStatus}
                    {downloading && progress > 0 && progress < 100 && <span> · {progress}%</span>}
                </div>

                {downloading && progress > 0 && (
                    <div style={{
                        width: "100%", height: scalePx(4),
                        background: "var(--color-bg-card)", borderRadius: scalePx(2),
                        marginBottom: scalePx(10), overflow: "hidden",
                    }}>
                        <div style={{
                            width: `${progress}%`, height: "100%",
                            background: progress === 100 ? "var(--color-success)" : "var(--color-primary)",
                            transition: "width 0.3s ease", borderRadius: scalePx(2),
                        }} />
                    </div>
                )}

                {models.length === 0 && !listLoading ? (
                    <Card>
                        <div style={{textAlign: "center", padding: scalePx(24), color: "var(--color-text-muted)", fontSize: scalePx(13)}}>
                            暂无远端模型
                        </div>
                    </Card>
                ) : (
                    <div style={{display: "flex", flexDirection: "column", gap: scalePx(8)}}>
                        {models.map(model => (
                            <Card key={model.name} marginBottom={0}>
                                <div style={{display: "flex", alignItems: "center", justifyContent: "space-between"}}>
                                    <div style={{flex: 1, minWidth: 0}}>
                                        <div style={{fontWeight: 600, fontSize: scalePx(14)}}>{model.name}</div>
                                        <div style={{fontSize: scalePx(11), color: "var(--color-text-dim)", marginTop: 2, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap"}}>
                                            {model.file} · {formatSize(model.size)} · <span style={{textTransform: "uppercase", fontSize: scalePx(10)}}>{model.type}</span>
                                        </div>
                                    </div>
                                    <ControlButton
                                        variant="success" size="small"
                                        onClick={() => downloadModel(model.name)}
                                        disabled={downloading !== null}
                                        loading={downloading === model.name}
                                    >
                                        {downloading === model.name ? `${progress}%` : "下载"}
                                    </ControlButton>
                                </div>
                            </Card>
                        ))}
                    </div>
                )}
            </div>
        </Page>
    );
};

export default DemoPage;
