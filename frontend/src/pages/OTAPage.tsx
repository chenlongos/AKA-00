import {useEffect, useRef, useState} from "react";
import {useViewportScale} from "../hooks/useViewportScale";
import Header from "../components/Header";
import Card from "../components/Card";
import ControlButton from "../components/ControlButton";
import ConfirmDialog from "../components/ConfirmDialog";

interface CheckResult {
    current_version?: string;
    latest_version?: string;
    update_available?: boolean;
    hardware_desc?: string;
    software_desc?: string;
    url?: string;
    message?: string;
}

const OTAPage = () => {
    const {scalePx} = useViewportScale();
    const [currentVersion, setCurrentVersion] = useState("加载中...");
    const [latestVersion, setLatestVersion] = useState("");
    const [hasUpdate, setHasUpdate] = useState(false);
    const [updateDesc, setUpdateDesc] = useState("");
    const [checking, setChecking] = useState(false);
    const [upgrading, setUpgrading] = useState(false);
    const [status, setStatus] = useState("");
    const [uploading, setUploading] = useState(false);
    const [restarting, setRestarting] = useState(false);
    const [showConfirm, setShowConfirm] = useState(false);
    const [upgradeProgress, setUpgradeProgress] = useState(0);
    const [upgradeMsg, setUpgradeMsg] = useState("");
    const intervalsRef = useRef<number[]>([]);

    const OTA_TASK_KEY = "ota_task";

    // Cleanup all intervals on unmount (prevent background polling on other pages)
    useEffect(() => {
        return () => intervalsRef.current.forEach(clearInterval);
    }, []);

    // Shared polling logic — used by both initial mount (resume) and new upgrade
    const startPolling = (taskId: string, type: "upgrade" | "upload") => {
        if (type === "upgrade") setUpgrading(true);
        else setUploading(true);

        const poll = setInterval(async () => {
            try {
                const pr = await fetch(`/api/ota/upgrade/progress?task_id=${taskId}`);
                const pd = await pr.json();
                setUpgradeProgress(pd.progress || 0);
                setUpgradeMsg(pd.message || "");

                if (pd.status === "done") {
                    clearInterval(poll);
                    intervalsRef.current = intervalsRef.current.filter(id => id !== poll);
                    setUpgradeProgress(100);
                    setUpgradeMsg("");
                    waitForService();
                } else if (pd.status === "error") {
                    clearInterval(poll);
                    intervalsRef.current = intervalsRef.current.filter(id => id !== poll);
                    setUpgrading(false);
                    setUploading(false);
                    setStatus(`升级失败: ${pd.message}`);
                    localStorage.removeItem(OTA_TASK_KEY);
                } else if (pd.status === "unknown") {
                    clearInterval(poll);
                    intervalsRef.current = intervalsRef.current.filter(id => id !== poll);
                    setUpgradeProgress(100);
                    setUpgradeMsg("");
                    waitForService();
                }
            } catch {
                clearInterval(poll);
                intervalsRef.current = intervalsRef.current.filter(id => id !== poll);
                setUpgradeProgress(100);
                setUpgradeMsg("");
                waitForService();
            }
        }, 500);
        intervalsRef.current.push(poll);
    };

    // After backend is killed for restart, poll /api/ota/version until it comes back
    const waitForService = () => {
        setUpgrading(false);
        setUploading(false);
        setRestarting(true);
        setUpgradeProgress(100);
        setUpgradeMsg("服务重启中...");
        localStorage.removeItem(OTA_TASK_KEY);
        let attempts = 0;
        let dots = 0;
        const check = setInterval(async () => {
            attempts++;
            dots = (dots + 1) % 4;
            setUpgradeMsg("服务重启中" + ".".repeat(dots));
            try {
                const r = await fetch("/api/ota/version");
                if (r.ok) {
                    clearInterval(check);
                    intervalsRef.current = intervalsRef.current.filter(id => id !== check);
                    setRestarting(false);
                    setStatus("更新完成！");
                    const d = await r.json();
                    if (d.version) setCurrentVersion(d.version);
                    setHasUpdate(false);
                    setLatestVersion("");
                }
            } catch {
                if (attempts > 60) {  // ~30s timeout
                    clearInterval(check);
                    intervalsRef.current = intervalsRef.current.filter(id => id !== check);
                    setRestarting(false);
                    setStatus("服务恢复超时，请手动刷新页面");
                }
            }
        }, 500);
        intervalsRef.current.push(check);
    };

    // On mount: check persistent OTA status and recover if needed
    useEffect(() => {
        fetch("/api/ota/status")
            .then(r => r.json())
            .then(s => {
                if (s.status === "completed") {
                    setStatus("更新完成！");
                    if (s.version) setCurrentVersion(s.version);
                    localStorage.removeItem(OTA_TASK_KEY);
                } else if (s.status === "downloading" || s.status === "installing") {
                    const saved = localStorage.getItem(OTA_TASK_KEY);
                    if (saved && s.task_id) {
                        try {
                            const {taskId, type} = JSON.parse(saved);
                            setStatus("检测到正在进行的更新...");
                            startPolling(taskId, type);
                        } catch { /* ignore parse error */ }
                    }
                }
            })
            .catch(() => {});

        fetch("/api/ota/version")
            .then(r => r.json())
            .then(d => { if (d.version) setCurrentVersion(d.version); })
            .catch(() => {});
    }, []);

    const checkUpdate = async () => {
        setChecking(true);
        setStatus("正在检查...");
        try {
            const res = await fetch("/api/ota/check");
            const data = await res.json() as CheckResult;
            if (data.current_version) setCurrentVersion(data.current_version);
            if (data.latest_version) setLatestVersion(data.latest_version);
            setHasUpdate(data.update_available === true);
            if (data.update_available) {
                const parts = [data.hardware_desc, data.software_desc].filter(Boolean);
                setUpdateDesc(parts.join("；") || "有新版本可用");
                setStatus("发现新版本");
            } else {
                setStatus("已是最新版本");
            }
        } catch {
            setStatus("检查失败");
        }
        finally { setChecking(false); }
    };

    const doUpgrade = () => { setShowConfirm(true); };

    const executeUpgrade = async () => {
        setShowConfirm(false);
        setUpgrading(true);
        setUpgradeProgress(0);
        setUpgradeMsg("正在启动升级...");
        setStatus("");

        try {
            const res = await fetch("/api/ota/upgrade", {method: "POST"});
            const data = await res.json();

            if (data.status !== "ok" || !data.task_id) {
                setStatus(`升级失败: ${data.message || "未知错误"}`);
                setUpgrading(false);
                return;
            }

            localStorage.setItem(OTA_TASK_KEY, JSON.stringify({taskId: data.task_id, type: "upgrade"}));
            startPolling(data.task_id, "upgrade");
        } catch {
            setUpgrading(false);
            setStatus("升级请求失败");
        }
    };

    const handleUpload = async (e: React.ChangeEvent<HTMLInputElement>) => {
        const file = e.target.files?.[0];
        if (!file) return;
        setUploading(true);
        setUpgradeProgress(0);
        setUpgradeMsg("正在上传文件...");
        setStatus("");
        try {
            const form = new FormData();
            form.append("firmware", file);
            const res = await fetch("/api/ota/update", {method: "POST", body: form});
            const data = await res.json();

            if (data.status !== "ok" || !data.task_id) {
                setStatus(`上传失败: ${data.message || "未知错误"}`);
                setUploading(false);
                return;
            }

            localStorage.setItem(OTA_TASK_KEY, JSON.stringify({taskId: data.task_id, type: "upload"}));
            startPolling(data.task_id, "upload");
        } catch (e) {
            setUploading(false);
            setStatus(`上传请求失败: ${e}`);
        }
    };

    return (
        <div style={{
            minHeight: "100dvh", overflowY: "auto", animation: "fadeIn 0.25s ease-out",
            "--color-bg": "#f2f2f7",
            "--color-bg-card": "#ffffff",
            "--color-bg-elevated": "#e5e5ea",
            "--color-text": "#1c1c1e",
            "--color-text-dim": "#8e8e93",
            "--color-text-muted": "#6e6e73",
            "--color-border": "#d1d1d6",
            "--color-border-light": "rgba(0,0,0,0.08)",
            "--color-primary": "#007aff",
            color: "#1c1c1e",
            background: "#f2f2f7",
        } as React.CSSProperties}>
            <Header title="固件升级" showClose closeTo="/settings" />

            <div style={{padding: scalePx(12)}}>
                {/* 版本信息 */}
                <Card marginBottom={12}>
                    <div style={{fontSize: scalePx(14)}}>
                        <div style={{display: "flex", justifyContent: "space-between", alignItems: "center"}}>
                            <span style={{color: "var(--color-text-dim)"}}>当前版本</span>
                            <span style={{fontWeight: 600, fontFamily: "monospace"}}>{currentVersion}</span>
                        </div>
                        {hasUpdate && (
                            <>
                                <div style={{display: "flex", justifyContent: "space-between", alignItems: "center", marginTop: scalePx(8)}}>
                                    <span style={{color: "var(--color-text-dim)"}}>最新版本</span>
                                    <span style={{fontWeight: 600, color: "var(--color-text)", fontFamily: "monospace"}}>{latestVersion}</span>
                                </div>
                                <div style={{marginTop: scalePx(6), fontSize: scalePx(13)}}>
                                    {updateDesc}
                                </div>
                            </>
                        )}
                    </div>
                </Card>

                {/* 在线升级 */}
                <Card marginBottom={12}>
                    <h3 style={{fontSize: scalePx(14), fontWeight: 600, marginBottom: scalePx(10)}}>在线升级</h3>
                    <div style={{display: "flex", gap: scalePx(8)}}>
                        <ControlButton variant="primary" size="full" onClick={checkUpdate} disabled={checking || restarting}>
                            {checking ? "检查中..." : "检查更新"}
                        </ControlButton>
                        {hasUpdate && (
                            <ControlButton variant="success" size="full" onClick={doUpgrade} disabled={upgrading || uploading || restarting}>
                                {upgrading ? "升级中..." : "立即升级"}
                            </ControlButton>
                        )}
                    </div>
                </Card>

                {/* 本地上传 */}
                <Card marginBottom={12}>
                    <h3 style={{fontSize: scalePx(14), fontWeight: 600, marginBottom: scalePx(10)}}>本地上传固件</h3>
                    <label style={{
                        display: "block", textAlign: "center",
                        padding: scalePx(14), borderRadius: scalePx(18),
                        background: "#007aff", color: "#fff",
                        fontWeight: "bold", fontSize: scalePx(16),
                        cursor: "pointer",
                        boxShadow: "0 6px 15px rgba(0,0,0,0.5)",
                    }}>
                        {uploading ? "上传中..." : restarting ? "重启中..." : "选择固件文件"}
                        <input type="file" onChange={handleUpload} style={{display: "none"}} disabled={uploading || upgrading || restarting} />
                    </label>
                </Card>

                {/* 升级进度条 */}
                {(upgrading || uploading || restarting) && (
                    <Card marginBottom={12}>
                        <div style={{textAlign: "center"}}>
                            <div style={{fontSize: scalePx(13), fontWeight: 600, marginBottom: scalePx(10), color: "var(--color-text)"}}>
                                {upgradeMsg || "正在升级..."}
                            </div>
                            <div style={{
                                width: "100%", height: scalePx(8),
                                background: "var(--color-bg-elevated)", borderRadius: scalePx(4),
                                overflow: "hidden",
                            }}>
                                <div style={{
                                    width: restarting ? "100%" : `${upgradeProgress}%`,
                                    height: "100%",
                                    background: restarting ? "var(--color-success)" :
                                        upgradeProgress === 100 ? "var(--color-success)" : "var(--color-primary)",
                                    borderRadius: scalePx(4),
                                    transition: "width 0.3s ease",
                                    ...(restarting ? {
                                        animation: "pulse 1.5s ease-in-out infinite",
                                    } : {}),
                                }} />
                            </div>
                            <div style={{fontSize: scalePx(12), color: "var(--color-text-dim)", marginTop: scalePx(6)}}>
                                {restarting ? "" : `${upgradeProgress}%`}
                            </div>
                        </div>
                    </Card>
                )}

                {/* 状态 */}
                {status && (
                    <div style={{
                        textAlign: "center", padding: scalePx(10), borderRadius: "var(--radius-sm)",
                        background: status.includes("成功") || status.includes("最新") ? "rgba(34,197,94,0.15)" : status.includes("失败") ? "rgba(239,68,68,0.15)" : "rgba(0,122,255,0.1)",
                        color: status.includes("成功") || status.includes("最新") ? "#16a34a" : status.includes("失败") ? "#dc2626" : "#007aff",
                        fontSize: scalePx(13), fontWeight: 500,
                    }}>
                        {status}
                    </div>
                )}
            </div>

            <ConfirmDialog
                open={showConfirm}
                title="确认升级"
                message="确定要升级固件吗？升级过程中请勿关闭电源。"
                confirmText="立即升级"
                danger
                onConfirm={executeUpgrade}
                onCancel={() => setShowConfirm(false)}
            />
        </div>
    );
};

export default OTAPage;
