import {useEffect, useState} from "react";
import {useViewportScale} from "../hooks/useViewportScale";
import Header from "../components/Header";
import Card from "../components/Card";
import ControlButton from "../components/ControlButton";

interface VersionInfo {
    version?: string;
    current?: string;
    latest?: string;
    message?: string;
}

const OTAPage = () => {
    const {scalePx} = useViewportScale();
    const [currentVersion, setCurrentVersion] = useState<string>("加载中...");
    const [latestVersion, setLatestVersion] = useState<string | null>(null);
    const [checking, setChecking] = useState(false);
    const [upgrading, setUpgrading] = useState(false);
    const [status, setStatus] = useState("");
    const [uploading, setUploading] = useState(false);

    useEffect(() => {
        fetchVersion();
    }, []);

    const fetchVersion = async () => {
        try {
            const res = await fetch("/api/ota/version");
            const data: VersionInfo = await res.json();
            setCurrentVersion(data.version || data.current || "未知");
        } catch {
            setCurrentVersion("获取失败");
        }
    };

    const checkUpdate = async () => {
        setChecking(true);
        setStatus("正在检查更新...");
        setLatestVersion(null);
        try {
            const res = await fetch("/api/ota/check");
            const data: VersionInfo = await res.json();
            if (data.latest) {
                setLatestVersion(data.latest);
                setStatus(data.latest !== currentVersion ? `发现新版本: ${data.latest}` : "已是最新版本");
            } else {
                setStatus(data.message || "检查完成");
            }
        } catch {
            setStatus("检查更新失败");
        }
        finally { setChecking(false); }
    };

    const doUpgrade = async () => {
        if (!confirm("确定要升级固件吗？升级过程中请勿关闭电源。")) return;
        setUpgrading(true);
        setStatus("正在下载并安装固件...");
        try {
            const res = await fetch("/api/ota/upgrade", {method: "POST"});
            const data = await res.json();
            if (data.status === "ok") {
                setStatus("固件升级成功，请重启设备");
            } else {
                setStatus(`升级失败: ${data.message || "未知错误"}`);
            }
        } catch {
            setStatus("升级请求失败");
        }
        finally { setUpgrading(false); }
    };

    const handleUpload = async (e: React.ChangeEvent<HTMLInputElement>) => {
        const file = e.target.files?.[0];
        if (!file) return;
        setUploading(true);
        setStatus("正在上传固件...");
        try {
            const form = new FormData();
            form.append("firmware", file);
            const res = await fetch("/api/ota/update", {method: "POST", body: form});
            const data = await res.json();
            if (data.status === "ok") {
                setStatus("固件上传成功，正在安装...");
            } else {
                setStatus(`上传失败: ${data.message || "未知错误"}`);
            }
        } catch {
            setStatus("上传请求失败");
        }
        finally { setUploading(false); }
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
                        {latestVersion && (
                            <div style={{display: "flex", justifyContent: "space-between", alignItems: "center", marginTop: scalePx(8)}}>
                                <span style={{color: "var(--color-text-dim)"}}>最新版本</span>
                                <span style={{fontWeight: 600, color: "var(--color-primary)", fontFamily: "monospace"}}>{latestVersion}</span>
                            </div>
                        )}
                    </div>
                </Card>

                {/* 在线升级 */}
                <Card marginBottom={12}>
                    <h3 style={{fontSize: scalePx(14), fontWeight: 600, marginBottom: scalePx(10)}}>在线升级</h3>
                    <div style={{display: "flex", gap: scalePx(8)}}>
                        <ControlButton variant="primary" size="full" onClick={checkUpdate} disabled={checking}>
                            {checking ? "检查中..." : "检查更新"}
                        </ControlButton>
                        {latestVersion && latestVersion !== currentVersion && (
                            <ControlButton variant="success" size="full" onClick={doUpgrade} disabled={upgrading}>
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
                        {uploading ? "上传中..." : "选择固件文件"}
                        <input type="file" accept=".zip,.tar.gz,.tgz" onChange={handleUpload} style={{display: "none"}} disabled={uploading} />
                    </label>
                </Card>

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
        </div>
    );
};

export default OTAPage;
