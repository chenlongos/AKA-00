import {useEffect, useRef, useState} from "react";
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
interface BasePwmChannels { left_ch1: number; left_ch2: number; right_ch1: number; right_ch2: number; }

const ArmAnglesPage = () => {
    const {scalePx} = useViewportScale();
    const [driver, setDriver] = useState("zp10s");
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
    const [pwm, setPwm] = useState<BasePwmChannels>({left_ch1: 0, left_ch2: 1, right_ch1: 2, right_ch2: 3});
    const [status, setStatus] = useState("");
    const [pwmStatus, setPwmStatus] = useState("");
    const [saving, setSaving] = useState(false);
    const [savingPwm, setSavingPwm] = useState(false);
    const previewTimerRef = useRef<number | null>(null);
    const requestIdRef = useRef(0);

    useEffect(() => {
        loadConfig(); loadPwm();
        return () => { if (previewTimerRef.current !== null) clearTimeout(previewTimerRef.current); };
    }, []);

    const currentAngles = driver === "zp10s" ? zp10s : sts;

    const loadConfig = async () => {
        try {
            const data = await api.arm.angles() as ArmAnglesResponse<ArmAnglesZP10S | ArmAnglesSTS>;
            if (data.driver === "zp10s") { setZp10s(data.angles as ArmAnglesZP10S); setDriver(data.driver); }
            else if (data.driver === "sts3215") { setSts(data.angles as ArmAnglesSTS); setDriver(data.driver); }
        } catch {}
    };
    const loadPwm = async () => {
        try { const d = await api.base.pwmChannels() as {pwm_channels: BasePwmChannels}; if (d.pwm_channels) setPwm(d.pwm_channels); } catch {}
    };

    const saveConfig = async () => {
        setSaving(true); setStatus("");
        try { const r = await api.arm.saveAngles(driver, currentAngles) as {error?: string}; setStatus(r.error ? "保存失败" : "保存成功"); if (!r.error) setTimeout(() => setStatus(""), 2000); }
        catch (e) { setStatus("请求失败: " + e); }
        finally { setSaving(false); }
    };
    const savePwm = async () => {
        setSavingPwm(true); setPwmStatus("");
        try { const r = await api.base.savePwmChannels(pwm) as {error?: string}; setPwmStatus(r.error ? "保存失败" : "PWM 通道已保存并生效"); if (!r.error) setTimeout(() => setPwmStatus(""), 2000); }
        catch (e) { setPwmStatus("请求失败: " + e); }
        finally { setSavingPwm(false); }
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
    const updPwm = (key: keyof BasePwmChannels, v: number) => setPwm(prev => ({...prev, [key]: v}));

    const statusBg = (s: string) => s.includes("成功") || s.includes("生效") ? "rgba(34,197,94,0.15)" : "rgba(239,68,68,0.15)";
    const statusClr = (s: string) => s.includes("成功") || s.includes("生效") ? "var(--color-success)" : "var(--color-danger)";

    const sectionTitle = (t: string) => (
        <div style={{fontSize: scalePx(10), fontWeight: 700, color: "var(--color-text-dim)", textTransform: "uppercase", letterSpacing: "1px", marginBottom: scalePx(8), marginTop: scalePx(4)}}>{t}</div>
    );

    const pwmRow = (key: keyof BasePwmChannels, label: string) => (
        <div key={key} style={{display: "flex", justifyContent: "space-between", alignItems: "center", padding: `${scalePx(9)} 0`, borderTop: "1px solid var(--color-border-light)", gap: scalePx(10)}}>
            <span style={{fontSize: scalePx(13)}}>{label}</span>
            <input type="number" min="0" step="1" value={pwm[key]}
                   onChange={e => updPwm(key, parseInt(e.target.value || "0", 10))}
                   style={{width: scalePx(68), border: "1px solid var(--color-border)", borderRadius: "var(--radius-sm)", padding: `${scalePx(5)} ${scalePx(7)}`, fontSize: scalePx(13), background: "var(--color-bg)", color: "var(--color-text)", outline: "none"}} />
        </div>
    );

    const swapBtn = (label: string, onClick: () => void) => (
        <button onClick={onClick} style={{padding: `${scalePx(7)} ${scalePx(8)}`, borderRadius: "var(--radius-sm)", border: "1px solid var(--color-border)", background: "var(--color-bg)", color: "var(--color-text)", cursor: "pointer", fontSize: scalePx(11)}}>
            {label}
        </button>
    );

    return (
        <div style={{
            minHeight: "100dvh", overflowY: "auto",
            // 恢复原始浅色主题（角度配置页原本是 iOS 风格浅色）
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
            <Header
                title="舵机角度配置"
                subtitle={<span>当前驱动: <b>{driver}</b></span>}
                showClose
                actions={
                    <button onClick={saveConfig} disabled={saving}
                            style={{color: saving ? "var(--color-text-dim)" : "var(--color-primary)", fontSize: scalePx(14), fontWeight: 600, background: "none", border: "none", cursor: saving ? "not-allowed" : "pointer"}}>
                        {saving ? "保存中..." : "保存"}
                    </button>
                }
            />

            {status && <div style={{textAlign: "center", padding: scalePx(8), background: statusBg(status), color: statusClr(status), fontSize: scalePx(12)}}>{status}</div>}

            <div style={{padding: scalePx(12)}}>
                {/* 角度配置 */}
                <Card marginBottom={12}>
                    <h3 style={{fontSize: scalePx(14), fontWeight: 700, marginBottom: scalePx(10)}}>
                        {driver === "zp10s" ? "ZP10S" : "STS3215"} 抓取序列
                    </h3>
                    {driver === "zp10s" ? (
                        <>
                            {sectionTitle("夹取阶段")}
                            <SliderRow label="舵机0 夹取角度" value={zp10s.servo0_prepare} min={0} max={270} onChange={v => updZ("servo0_prepare", v)} suffix="°" />
                            <SliderRow label="舵机1 夹取角度" value={zp10s.servo1_prepare} min={0} max={270} onChange={v => updZ("servo1_prepare", v)} suffix="°" />
                            <SliderRow label="夹爪张开角度" value={zp10s.servo2_prepare} min={0} max={270} onChange={v => updZ("servo2_prepare", v)} suffix="°" />
                            {sectionTitle("进入阶段")}
                            <SliderRow label="夹爪进入角度" value={zp10s.servo2_approach} min={0} max={270} onChange={v => updZ("servo2_approach", v)} suffix="°" />
                            <SliderRow label="夹爪闭合角度" value={zp10s.servo2_grab} min={0} max={270} onChange={v => updZ("servo2_grab", v)} suffix="°" />
                            {sectionTitle("抬起阶段")}
                            <SliderRow label="舵机0 抬起角度" value={zp10s.servo0_lift} min={0} max={270} onChange={v => updZ("servo0_lift", v)} suffix="°" />
                            <SliderRow label="舵机1 抬起角度" value={zp10s.servo1_lift} min={0} max={270} onChange={v => updZ("servo1_lift", v)} suffix="°" />
                            <SliderRow label="舵机2 抬起角度" value={zp10s.servo2_lift} min={0} max={270} onChange={v => updZ("servo2_lift", v)} suffix="°" />
                        </>
                    ) : (
                        <>
                            {sectionTitle("准备阶段")}
                            <SliderRow label="舵机1 准备角度" value={sts.servo1_prepare} min={0} max={4095} onChange={v => updS("servo1_prepare", v)} />
                            <SliderRow label="舵机2 准备角度" value={sts.servo2_prepare} min={0} max={4095} onChange={v => updS("servo2_prepare", v)} />
                            <SliderRow label="舵机3 准备角度 (张开)" value={sts.servo3_prepare} min={0} max={4095} onChange={v => updS("servo3_prepare", v)} />
                            {sectionTitle("进入阶段")}
                            <SliderRow label="舵机1 进入角度" value={sts.servo1_enter} min={0} max={4095} onChange={v => updS("servo1_enter", v)} />
                            <SliderRow label="舵机2 进入角度" value={sts.servo2_enter} min={0} max={4095} onChange={v => updS("servo2_enter", v)} />
                            <SliderRow label="舵机3 进入角度" value={sts.servo3_enter} min={0} max={4095} onChange={v => updS("servo3_enter", v)} />
                            {sectionTitle("抓取阶段")}
                            <SliderRow label="舵机3 抓取角度 (闭合)" value={sts.servo3_grab} min={0} max={4095} onChange={v => updS("servo3_grab", v)} />
                            {sectionTitle("抬起阶段")}
                            <SliderRow label="舵机1 抬起角度" value={sts.servo1_lift} min={0} max={4095} onChange={v => updS("servo1_lift", v)} />
                            <SliderRow label="舵机2 抬起角度" value={sts.servo2_lift} min={0} max={4095} onChange={v => updS("servo2_lift", v)} />
                            <SliderRow label="舵机3 抬起角度" value={sts.servo3_lift} min={0} max={4095} onChange={v => updS("servo3_lift", v)} />
                        </>
                    )}
                </Card>

                {/* PWM 通道（暂时隐藏） */}
                {false && (
                <Card marginBottom={0}>
                    <div style={{display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: scalePx(10)}}>
                        <div>
                            <h3 style={{fontSize: scalePx(14), fontWeight: 700, margin: 0}}>底盘 PWM 通道</h3>
                            <div style={{fontSize: scalePx(10), color: "var(--color-text-dim)", marginTop: 2}}>base_pwm_channels.json</div>
                        </div>
                        <button onClick={savePwm} disabled={savingPwm}
                                style={{color: savingPwm ? "var(--color-text-dim)" : "var(--color-primary)", fontSize: scalePx(14), fontWeight: 600, background: "none", border: "none", cursor: savingPwm ? "not-allowed" : "pointer"}}>
                            {savingPwm ? "保存中..." : "保存 PWM"}
                        </button>
                    </div>
                    {pwmStatus && <div style={{textAlign: "center", padding: scalePx(8), marginBottom: scalePx(8), borderRadius: "var(--radius-sm)", background: statusBg(pwmStatus), color: statusClr(pwmStatus), fontSize: scalePx(12)}}>{pwmStatus}</div>}
                    <div style={{display: "grid", gridTemplateColumns: "repeat(2, 1fr)", gap: scalePx(7), marginBottom: scalePx(10)}}>
                        {swapBtn("切换左右轮", () => setPwm(prev => ({left_ch1: prev.right_ch1, left_ch2: prev.right_ch2, right_ch1: prev.left_ch1, right_ch2: prev.left_ch2})))}
                        {swapBtn("左轮前后切换", () => setPwm(prev => ({...prev, left_ch1: prev.left_ch2, left_ch2: prev.left_ch1})))}
                        {swapBtn("右轮前后切换", () => setPwm(prev => ({...prev, right_ch1: prev.right_ch2, right_ch2: prev.right_ch1})))}
                    </div>
                    {pwmRow("left_ch1", "左轮前进 ch1")}
                    {pwmRow("left_ch2", "左轮后退 ch2")}
                    {pwmRow("right_ch1", "右轮前进 ch1")}
                    {pwmRow("right_ch2", "右轮后退 ch2")}
                </Card>
                )}
            </div>
        </div>
    );
};

export default ArmAnglesPage;
