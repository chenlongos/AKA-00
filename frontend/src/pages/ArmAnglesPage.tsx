import {useEffect, useRef, useState} from "react";
import {useLocation} from "react-router-dom";
import {useViewportScale} from "../hooks/useViewportScale";
import {api} from "../api";
import Header from "../components/Header";
import Card from "../components/Card";
import SliderRow from "../components/SliderRow";

interface ArmAnglesZP10S {
    servo0_prepare: number; servo1_prepare: number; servo2_prepare: number;
    servo2_approach: number; servo2_grab: number;
    servo0_lift: number; servo1_lift: number; servo2_lift: number;
}
interface ArmAnglesSTS {
    servo1_prepare: number; servo2_prepare: number; servo3_prepare: number;
    servo1_enter: number; servo2_enter: number; servo3_enter: number;
    servo3_grab: number;
    servo1_lift: number; servo2_lift: number; servo3_lift: number;
}
interface ArmAnglesResponse<T> { driver: string; angles: T; }

const ArmAnglesPage = () => {
    const {scalePx} = useViewportScale();
    const [driver, setZp] = useState("zp10s");
    const [zp10s, setZp10s] = useState<ArmAnglesZP10S>({
        servo0_prepare: 245, servo1_prepare: 180, servo2_prepare: 150,
        servo2_approach: 150, servo2_grab: 90,
        servo0_lift: 200, servo1_lift: 180, servo2_lift: 90,
    });
    const [sts, setSts] = useState<ArmAnglesSTS>({
        servo1_prepare: 2300, servo2_prepare: 2100, servo3_prepare: 4000,
        servo1_enter: 1850, servo2_enter: 2650, servo3_enter: 4000,
        servo3_grab: 3000,
        servo1_lift: 2300, servo2_lift: 2100, servo3_lift: 3000,
    });
    const [status, setStatus] = useState("");
    const [saving, setSaving] = useState(false);
    const location = useLocation();
    const isAdmin = location.pathname.endsWith("/admin");
    const previewTimerRef = useRef<number | null>(null);
    const requestIdRef = useRef(0);

    useEffect(() => {
        loadConfig();
        return () => { if (previewTimerRef.current !== null) clearTimeout(previewTimerRef.current); };
    }, []);

    const currentAngles = driver === "zp10s" ? zp10s : sts;

    const loadConfig = async () => {
        try {
            const data = await (isAdmin ? api.arm.defaultAngles() : api.arm.angles()) as ArmAnglesResponse<ArmAnglesZP10S | ArmAnglesSTS>;
            if (data.driver === "zp10s") { setZp10s(data.angles as ArmAnglesZP10S); setZp(data.driver); }
            else if (data.driver === "sts3215") { setSts(data.angles as ArmAnglesSTS); setZp(data.driver); }
        } catch {}
    };

    const saveConfig = async () => {
        setSaving(true); setStatus("");
        const save = isAdmin ? api.arm.saveDefaultAngles : api.arm.saveAngles;
        try { const r = await save(driver, currentAngles) as {error?: string}; setStatus(r.error ? "保存失败" : (isAdmin ? "出厂默认已保存" : "保存成功")); if (!r.error) setTimeout(() => setStatus(""), 2000); }
        catch (e) { setStatus("请求失败: " + e); }
        finally { setSaving(false); }
    };

    const queuePreview = (key: string, value: number, nextAngles: ArmAnglesZP10S | ArmAnglesSTS) => {
        if (previewTimerRef.current !== null) clearTimeout(previewTimerRef.current);
        setStatus(`已调整 ${key}: ${value}`);
        previewTimerRef.current = window.setTimeout(async () => {
            const reqId = ++requestIdRef.current; setSaving(true);
            try { const r = await api.arm.preview(driver, key, value, nextAngles); if (r?.error) throw new Error(r.error); if (reqId === requestIdRef.current) setStatus(`已同步 ${key}: ${value}`); }
            catch (e) { if (reqId === requestIdRef.current) setStatus("同步失败: " + e); }
            finally { if (reqId === requestIdRef.current) setSaving(false); }
        }, 80);
    };

    const updZ = (key: keyof ArmAnglesZP10S, v: number) => setZp10s(prev => { const n = {...prev, [key]: v}; queuePreview(key, v, n); return n; });
    const updS = (key: keyof ArmAnglesSTS, v: number) => setSts(prev => { const n = {...prev, [key]: v}; queuePreview(key, v, n); return n; });

    const statusBg = (s: string) => s.includes("成功") || s.includes("生效") ? "rgba(34,197,94,0.15)" : "rgba(239,68,68,0.15)";
    const statusClr = (s: string) => s.includes("成功") || s.includes("生效") ? "var(--color-success)" : "var(--color-danger)";
    const sectionTitle = (t: string) => (
        <div style={{fontSize: scalePx(10), fontWeight: 700, color: "var(--color-text-dim)", textTransform: "uppercase", letterSpacing: "1px", marginBottom: scalePx(8), marginTop: scalePx(4)}}>{t}</div>
    );

    return (
        <div style={{
            minHeight: "100dvh", overflowY: "auto",
            "--color-bg": "#f2f2f7", "--color-bg-card": "#ffffff",
            "--color-bg-elevated": "#e5e5ea", "--color-text": "#1c1c1e",
            "--color-text-dim": "#8e8e93", "--color-text-muted": "#6e6e73",
            "--color-border": "#d1d1d6", "--color-border-light": "rgba(0,0,0,0.08)",
            "--color-primary": "#007aff",
            color: "#1c1c1e", background: "#f2f2f7",
        } as React.CSSProperties}>
            <Header
                title={isAdmin ? "出厂默认角度" : "舵机角度配置"}
                subtitle={<span>当前驱动: <b>{driver}</b></span>}
                showClose closeTo="/settings"
                actions={
                    <div style={{display: "flex", gap: scalePx(12), alignItems: "center"}}>
                        {!isAdmin && (
                            <button onClick={async () => {
                                try {
                                    const def = await api.arm.defaultAngles() as {angles?: object};
                                    if (def.angles) {
                                        if (driver === "zp10s") setZp10s(def.angles as ArmAnglesZP10S);
                                        else setSts(def.angles as ArmAnglesSTS);
                                        setStatus("已恢复默认值，请点击保存");
                                        setTimeout(() => setStatus(""), 3000);
                                    }
                                } catch { setStatus("恢复失败"); }
                            }} style={{color: "var(--color-text-dim)", fontSize: scalePx(13), fontWeight: 400, background: "none", border: "none", cursor: "pointer"}}>
                                恢复默认
                            </button>
                        )}
                        <button onClick={saveConfig} disabled={saving}
                                style={{color: saving ? "var(--color-text-dim)" : "var(--color-primary)", fontSize: scalePx(14), fontWeight: 600, background: "none", border: "none", cursor: saving ? "not-allowed" : "pointer"}}>
                            {saving ? "保存中..." : "保存"}
                        </button>
                    </div>
                }
            />

            {status && <div style={{textAlign: "center", padding: scalePx(8), background: statusBg(status), color: statusClr(status), fontSize: scalePx(12)}}>{status}</div>}

            <div style={{padding: scalePx(12)}}>
                <Card>
                    <div style={{padding: scalePx(4)}}>
                        {sectionTitle("角度配置")}
                        {driver === "zp10s" ? (
                            <>
                                <SliderRow label="Servo0 准备位" value={zp10s.servo0_prepare} min={0} max={270} onChange={v => updZ("servo0_prepare", v)} />
                                <SliderRow label="Servo1 准备位" value={zp10s.servo1_prepare} min={0} max={270} onChange={v => updZ("servo1_prepare", v)} />
                                <SliderRow label="Servo2 准备位" value={zp10s.servo2_prepare} min={0} max={270} onChange={v => updZ("servo2_prepare", v)} />
                                <SliderRow label="Servo2 接近位" value={zp10s.servo2_approach} min={0} max={270} onChange={v => updZ("servo2_approach", v)} />
                                <SliderRow label="Servo2 抓取位" value={zp10s.servo2_grab} min={0} max={270} onChange={v => updZ("servo2_grab", v)} />
                                <SliderRow label="Servo0 抬起位" value={zp10s.servo0_lift} min={0} max={270} onChange={v => updZ("servo0_lift", v)} />
                                <SliderRow label="Servo1 抬起位" value={zp10s.servo1_lift} min={0} max={270} onChange={v => updZ("servo1_lift", v)} />
                                <SliderRow label="Servo2 抬起位" value={zp10s.servo2_lift} min={0} max={270} onChange={v => updZ("servo2_lift", v)} />
                            </>
                        ) : (
                            <>
                                <SliderRow label="Servo1 准备位" value={sts.servo1_prepare} min={0} max={4095} onChange={v => updS("servo1_prepare", v)} />
                                <SliderRow label="Servo2 准备位" value={sts.servo2_prepare} min={0} max={4095} onChange={v => updS("servo2_prepare", v)} />
                                <SliderRow label="Servo3 准备位" value={sts.servo3_prepare} min={0} max={4095} onChange={v => updS("servo3_prepare", v)} />
                                <SliderRow label="Servo1 进入位" value={sts.servo1_enter} min={0} max={4095} onChange={v => updS("servo1_enter", v)} />
                                <SliderRow label="Servo2 进入位" value={sts.servo2_enter} min={0} max={4095} onChange={v => updS("servo2_enter", v)} />
                                <SliderRow label="Servo3 进入位" value={sts.servo3_enter} min={0} max={4095} onChange={v => updS("servo3_enter", v)} />
                                <SliderRow label="Servo3 抓取位" value={sts.servo3_grab} min={0} max={4095} onChange={v => updS("servo3_grab", v)} />
                                <SliderRow label="Servo1 抬起位" value={sts.servo1_lift} min={0} max={4095} onChange={v => updS("servo1_lift", v)} />
                                <SliderRow label="Servo2 抬起位" value={sts.servo2_lift} min={0} max={4095} onChange={v => updS("servo2_lift", v)} />
                                <SliderRow label="Servo3 抬起位" value={sts.servo3_lift} min={0} max={4095} onChange={v => updS("servo3_lift", v)} />
                            </>
                        )}
                    </div>
                </Card>
            </div>
        </div>
    );
};

export default ArmAnglesPage;
