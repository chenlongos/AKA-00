import {useEffect, useRef, useState} from "react";
import {useLocation} from "react-router-dom";
import {useViewportScale} from "../hooks/useViewportScale";
import {api} from "../api";
import Header from "../components/Header";
import Card from "../components/Card";
import SliderRow from "../components/SliderRow";

/** 新语义化角度配置结构 */
interface ArmAnglesNew {
    grab_position: Record<string, number>;
    lift_position: Record<string, number>;
    gripper_open: number;
    gripper_close: number;
}

/** servo key 列表（按 driver 区分，不含夹爪舵机） */
const ZP10S_SERVOS = ["servo0", "servo1"];
const STS_SERVOS = ["servo1", "servo2"];

const ZP10S_DEFAULTS: ArmAnglesNew = {
    grab_position: {servo0: 245, servo1: 180},
    lift_position: {servo0: 200, servo1: 180},
    gripper_open: 150,
    gripper_close: 90,
};

type Driver = "zp10s" | "sts3215";

const ArmAnglesPage = () => {
    const {scalePx} = useViewportScale();
    const [driver, setDriver] = useState<Driver>("zp10s");
    const [angles, setAngles] = useState<ArmAnglesNew>(ZP10S_DEFAULTS);
    const [status, setStatus] = useState("");
    const [saving, setSaving] = useState(false);
    const location = useLocation();
    const isAdmin = location.pathname.endsWith("/admin");
    const previewTimerRef = useRef<number | null>(null);
    const requestIdRef = useRef(0);

    const servoKeys = driver === "zp10s" ? ZP10S_SERVOS : STS_SERVOS;
    const angleRange = driver === "zp10s" ? {min: 0, max: 270} : {min: 0, max: 4095};

    useEffect(() => {
        loadConfig();
        return () => { if (previewTimerRef.current !== null) clearTimeout(previewTimerRef.current); };
    }, []);

    const loadConfig = async () => {
        try {
            const data = await (isAdmin ? api.arm.defaultAngles() : api.arm.angles()) as {driver: string; angles: ArmAnglesNew};
            if (data.driver === "zp10s" || data.driver === "sts3215") {
                setDriver(data.driver as Driver);
                setAngles(data.angles);
            }
        } catch {}
    };

    const saveConfig = async () => {
        setSaving(true); setStatus("");
        const save = isAdmin ? api.arm.saveDefaultAngles : api.arm.saveAngles;
        try {
            const r = await save(driver, angles) as {error?: string};
            setStatus(r.error ? "保存失败" : (isAdmin ? "出厂默认已保存" : "保存成功"));
            if (!r.error) setTimeout(() => setStatus(""), 2000);
        } catch (e) {
            setStatus("请求失败: " + e);
        } finally {
            setSaving(false);
        }
    };

    /** 更新 grab_position 中某个 servo */
    const updGrab = (servoKey: string, value: number) => {
        setAngles(prev => {
            const next: ArmAnglesNew = {
                ...prev,
                grab_position: {...prev.grab_position, [servoKey]: value},
            };
            queuePreview(`grab_position.${servoKey}`, value, next);
            return next;
        });
    };

    /** 更新 lift_position 中某个 servo */
    const updLift = (servoKey: string, value: number) => {
        setAngles(prev => {
            const next: ArmAnglesNew = {
                ...prev,
                lift_position: {...prev.lift_position, [servoKey]: value},
            };
            queuePreview(`lift_position.${servoKey}`, value, next);
            return next;
        });
    };

    /** 更新 gripper_open / gripper_close */
    const updScalar = (key: "gripper_open" | "gripper_close", value: number) => {
        setAngles(prev => {
            const next: ArmAnglesNew = {...prev, [key]: value};
            queuePreview(key, value, next);
            return next;
        });
    };

    const queuePreview = (key: string, value: number, nextAngles: ArmAnglesNew) => {
        if (previewTimerRef.current !== null) clearTimeout(previewTimerRef.current);
        setStatus(`已调整 ${key}: ${value}`);
        previewTimerRef.current = window.setTimeout(async () => {
            const reqId = ++requestIdRef.current; setSaving(true);
            try {
                const r = await api.arm.preview(driver, key, value, nextAngles);
                if (r?.error) throw new Error(r.error);
                if (reqId === requestIdRef.current) setStatus(`已同步 ${key}: ${value}`);
            } catch (e) {
                if (reqId === requestIdRef.current) setStatus("同步失败: " + e);
            } finally {
                if (reqId === requestIdRef.current) setSaving(false);
            }
        }, 80);
    };

    const statusBg = (s: string) => s.includes("成功") || s.includes("生效") ? "rgba(34,197,94,0.15)" : "rgba(239,68,68,0.15)";
    const statusClr = (s: string) => s.includes("成功") || s.includes("生效") ? "var(--color-success)" : "var(--color-danger)";

    const sectionTitle = (t: string, subtitle?: string) => (
        <div style={{marginBottom: scalePx(8), marginTop: scalePx(4)}}>
            <div style={{fontSize: scalePx(10), fontWeight: 700, color: "var(--color-text-dim)", textTransform: "uppercase", letterSpacing: "1px"}}>{t}</div>
            {subtitle && <div style={{fontSize: scalePx(9), color: "var(--color-text-muted)", marginTop: scalePx(2)}}>{subtitle}</div>}
        </div>
    );

    /** servo ID 对应的中文标签 */
    const servoLabel = (sk: string, driver: Driver) => {
        if (driver === "zp10s") {
            const map: Record<string, string> = {servo0: "底座旋转", servo1: "腕部"};
            return map[sk] || sk;
        }
        const map: Record<string, string> = {servo1: "底座旋转", servo2: "腕部"};
        return map[sk] || sk;
    };

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
                                    const def = await api.arm.defaultAngles() as {angles?: ArmAnglesNew};
                                    if (def.angles) {
                                        setAngles(def.angles);
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
                {/* 夹取位置 */}
                <Card>
                    <div style={{padding: scalePx(4)}}>
                        {sectionTitle("夹取位置", "抓取时各舵机的目标角度")}
                        {servoKeys.map(sk => (
                            <SliderRow
                                key={`grab-${sk}`}
                                label={`${servoLabel(sk, driver)} (${sk})`}
                                value={angles.grab_position[sk] ?? 0}
                                min={angleRange.min}
                                max={angleRange.max}
                                onChange={v => updGrab(sk, v)}
                            />
                        ))}
                    </div>
                </Card>

                {/* 抬起位置 */}
                <Card>
                    <div style={{padding: scalePx(4)}}>
                        {sectionTitle("抬起位置", "抓取后抬起时各舵机的目标角度")}
                        {servoKeys.map(sk => (
                            <SliderRow
                                key={`lift-${sk}`}
                                label={`${servoLabel(sk, driver)} (${sk})`}
                                value={angles.lift_position[sk] ?? 0}
                                min={angleRange.min}
                                max={angleRange.max}
                                onChange={v => updLift(sk, v)}
                            />
                        ))}
                    </div>
                </Card>

                {/* 夹爪 */}
                <Card>
                    <div style={{padding: scalePx(4)}}>
                        {sectionTitle("夹爪开闭", "夹爪张开和闭合的角度")}
                        <SliderRow
                            label="夹爪张开"
                            value={angles.gripper_open}
                            min={angleRange.min}
                            max={angleRange.max}
                            onChange={v => updScalar("gripper_open", v)}
                        />
                        <SliderRow
                            label="夹爪闭合"
                            value={angles.gripper_close}
                            min={angleRange.min}
                            max={angleRange.max}
                            onChange={v => updScalar("gripper_close", v)}
                        />
                    </div>
                </Card>
            </div>
        </div>
    );
};

export default ArmAnglesPage;
