import {useEffect, useRef, useState} from "react";

interface ArmAnglesZP10S {
    servo0_prepare: number;
    servo1_prepare: number;
    servo2_prepare: number;
    servo2_approach: number;
    servo2_grab: number;
    servo0_lift: number;
    servo1_lift: number;
    servo2_lift: number;
}

interface ArmAnglesSTS {
    servo1_prepare: number;
    servo2_prepare: number;
    servo3_prepare: number;
    servo1_enter: number;
    servo2_enter: number;
    servo3_enter: number;
    servo3_grab: number;
    servo1_lift: number;
    servo2_lift: number;
    servo3_lift: number;
}

interface ArmAnglesResponse<T> {
    driver: string;
    angles: T;
}

interface BasePwmChannels {
    left_ch1: number;
    left_ch2: number;
    right_ch1: number;
    right_ch2: number;
}

const ArmAnglesPage = () => {
    const [driver, setDriver] = useState<string>("zp10s");
    const [zp10s, setZp10s] = useState<ArmAnglesZP10S>({
        servo0_prepare: 245,
        servo1_prepare: 180,
        servo2_prepare: 150,
        servo2_approach: 150,
        servo2_grab: 90,
        servo0_lift: 200,
        servo1_lift: 180,
        servo2_lift: 90,
    });
    const [sts, setSts] = useState<ArmAnglesSTS>({
        servo1_prepare: 2300,
        servo2_prepare: 2100,
        servo3_prepare: 4000,
        servo1_enter: 1850,
        servo2_enter: 2650,
        servo3_enter: 4000,
        servo3_grab: 3000,
        servo1_lift: 2300,
        servo2_lift: 2100,
        servo3_lift: 3000,
    });
    const [basePwmChannels, setBasePwmChannels] = useState<BasePwmChannels>({
        left_ch1: 0,
        left_ch2: 1,
        right_ch1: 2,
        right_ch2: 3,
    });
    const [status, setStatus] = useState<string>("");
    const [pwmStatus, setPwmStatus] = useState<string>("");
    const [saving, setSaving] = useState(false);
    const [savingPwm, setSavingPwm] = useState(false);
    const previewTimerRef = useRef<number | null>(null);
    const requestIdRef = useRef(0);

    useEffect(() => {
        loadConfig();
        loadBasePwmChannels();
        return () => {
            if (previewTimerRef.current !== null) {
                window.clearTimeout(previewTimerRef.current);
            }
        };
    }, []);

    const currentAngles = driver === "zp10s" ? zp10s : sts;

    const loadConfig = async () => {
        try {
            const r = await fetch("/api/arm_angles");
            const data = await r.json() as ArmAnglesResponse<ArmAnglesZP10S | ArmAnglesSTS>;
            if (data.driver === "zp10s") {
                setZp10s(data.angles as ArmAnglesZP10S);
                setDriver(data.driver);
            } else if (data.driver === "sts3215") {
                setSts(data.angles as ArmAnglesSTS);
                setDriver(data.driver);
            }
        } catch (e) {
            console.error("加载配置失败", e);
        }
    };

    const loadBasePwmChannels = async () => {
        try {
            const r = await fetch("/api/base_pwm_channels");
            const data = await r.json() as {pwm_channels: BasePwmChannels};
            if (data.pwm_channels) {
                setBasePwmChannels(data.pwm_channels);
            }
        } catch (e) {
            console.error("加载 PWM 通道配置失败", e);
        }
    };

    const saveConfig = async () => {
        setSaving(true);
        setStatus("");
        try {
            const r = await fetch("/api/arm_angles", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({
                    driver,
                    angles: currentAngles,
                }),
            });
            if (r.ok) {
                setStatus("保存成功");
                setTimeout(() => setStatus(""), 2000);
            } else {
                setStatus("保存失败");
            }
        } catch (e) {
            setStatus("请求失败: " + e);
        } finally {
            setSaving(false);
        }
    };

    const savePwmChannels = async () => {
        setSavingPwm(true);
        setPwmStatus("");
        try {
            const r = await fetch("/api/base_pwm_channels", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({pwm_channels: basePwmChannels}),
            });
            if (r.ok) {
                setPwmStatus("PWM 通道已保存并生效");
                setTimeout(() => setPwmStatus(""), 2000);
            } else {
                setPwmStatus("保存失败");
            }
        } catch (e) {
            setPwmStatus("请求失败: " + e);
        } finally {
            setSavingPwm(false);
        }
    };

    const queuePreviewSave = (key: string, value: number, nextAngles: ArmAnglesZP10S | ArmAnglesSTS) => {
        if (previewTimerRef.current !== null) {
            window.clearTimeout(previewTimerRef.current);
        }

        setStatus(`已调整 ${key}: ${value}`);
        previewTimerRef.current = window.setTimeout(async () => {
            const requestId = ++requestIdRef.current;
            setSaving(true);
            try {
                const r = await fetch("/api/arm_angles/preview", {
                    method: "POST",
                    headers: {"Content-Type": "application/json"},
                    body: JSON.stringify({
                        driver,
                        key,
                        value,
                        angles: nextAngles,
                    }),
                });
                if (!r.ok) {
                    throw new Error(await r.text());
                }
                if (requestId === requestIdRef.current) {
                    setStatus(`已同步并预览 ${key}: ${value}`);
                }
            } catch (e) {
                if (requestId === requestIdRef.current) {
                    setStatus("同步失败: " + e);
                }
            } finally {
                if (requestId === requestIdRef.current) {
                    setSaving(false);
                }
            }
        }, 80);
    };

    const updateZp10s = (key: keyof ArmAnglesZP10S, value: number) => {
        setZp10s((prev) => {
            const next = {...prev, [key]: value};
            queuePreviewSave(key, value, next);
            return next;
        });
    };

    const updateSts = (key: keyof ArmAnglesSTS, value: number) => {
        setSts((prev) => {
            const next = {...prev, [key]: value};
            queuePreviewSave(key, value, next);
            return next;
        });
    };

    const updateBasePwmChannels = (key: keyof BasePwmChannels, value: number) => {
        setBasePwmChannels((prev) => ({...prev, [key]: value}));
    };

    const swapLeftRightWheels = () => {
        setBasePwmChannels((prev) => ({
            left_ch1: prev.right_ch1,
            left_ch2: prev.right_ch2,
            right_ch1: prev.left_ch1,
            right_ch2: prev.left_ch2,
        }));
    };

    const swapLeftWheelDirection = () => {
        setBasePwmChannels((prev) => ({
            ...prev,
            left_ch1: prev.left_ch2,
            left_ch2: prev.left_ch1,
        }));
    };

    const swapRightWheelDirection = () => {
        setBasePwmChannels((prev) => ({
            ...prev,
            right_ch1: prev.right_ch2,
            right_ch2: prev.right_ch1,
        }));
    };

    return (
        <div
            style={{
                fontFamily: "-apple-system, sans-serif",
                background: "#f2f2f7",
                minHeight: "100vh",
                color: "#1c1c1e",
            }}
        >
            {/* Header */}
            <div
                style={{
                    background: "#fff",
                    padding: "16px 20px",
                    borderBottom: "0.5px solid #c6c6c8",
                    display: "flex",
                    justifyContent: "space-between",
                    alignItems: "center",
                    position: "sticky",
                    top: 0,
                    zIndex: 100,
                }}
            >
                <div style={{display: "flex", flexDirection: "column"}}>
                    <h1 style={{fontSize: 17, margin: 0, fontWeight: 600}}>舵机角度配置</h1>
                    <div style={{fontSize: 12, color: "#8e8e93", marginTop: 2}}>
                        当前驱动: <b>{driver}</b>
                    </div>
                </div>
                <button
                    onClick={saveConfig}
                    disabled={saving}
                    style={{
                        color: saving ? "#8e8e93" : "#007aff",
                        fontSize: 15,
                        cursor: saving ? "not-allowed" : "pointer",
                        background: "none",
                        border: "none",
                        fontWeight: 600,
                    }}
                >
                    {saving ? "保存中..." : "保存"}
                </button>
            </div>

            {/* Status */}
            {status && (
                <div
                    style={{
                        textAlign: "center",
                        padding: 10,
                        background: status.includes("成功") ? "#d4edda" : "#f8d7da",
                        color: status.includes("成功") ? "#155724" : "#721c24",
                    }}
                >
                    {status}
                </div>
            )}

            {/* ZP10S Config */}
            {driver === "zp10s" && (
                <div style={{padding: 16}}>
                    <div
                        style={{
                            background: "#fff",
                            borderRadius: 10,
                            padding: 16,
                            boxShadow: "0 1px 2px rgba(0,0,0,0.05)",
                        }}
                    >
                        <h3 style={{fontSize: 15, fontWeight: 600, marginBottom: 16}}>ZP10S 抓取序列</h3>

                        <div style={{marginBottom: 20}}>
                            <div style={{fontSize: 13, fontWeight: 600, color: "#8e8e93", marginBottom: 12}}>夹取阶段</div>
                            <div style={{marginBottom: 16}}>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机0 夹取角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{zp10s.servo0_prepare}°</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="270"
                                    value={zp10s.servo0_prepare}
                                    onChange={(e) => updateZp10s("servo0_prepare", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>

                            <div style={{marginBottom: 16}}>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机1 夹取角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{zp10s.servo1_prepare}°</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="270"
                                    value={zp10s.servo1_prepare}
                                    onChange={(e) => updateZp10s("servo1_prepare", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>

                            <div>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>夹爪张开角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{zp10s.servo2_prepare}°</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="270"
                                    value={zp10s.servo2_prepare}
                                    onChange={(e) => updateZp10s("servo2_prepare", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>
                        </div>

                        <div style={{marginBottom: 20}}>
                            <div style={{fontSize: 13, fontWeight: 600, color: "#8e8e93", marginBottom: 12}}>进入阶段</div>
                            <div>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>夹爪夹取过程角度 (保持张开)</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{zp10s.servo2_approach}°</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="270"
                                    value={zp10s.servo2_approach}
                                    onChange={(e) => updateZp10s("servo2_approach", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>
                            <div>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>夹爪闭合角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{zp10s.servo2_grab}°</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="270"
                                    value={zp10s.servo2_grab}
                                    onChange={(e) => updateZp10s("servo2_grab", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>
                        </div>

                        <div>
                            <div style={{fontSize: 13, fontWeight: 600, color: "#8e8e93", marginBottom: 12}}>抬起阶段</div>
                            <div style={{marginBottom: 16}}>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机0 抬起角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{zp10s.servo0_lift}°</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="270"
                                    value={zp10s.servo0_lift}
                                    onChange={(e) => updateZp10s("servo0_lift", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>

                            <div style={{marginBottom: 16}}>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机1 抬起角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{zp10s.servo1_lift}°</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="270"
                                    value={zp10s.servo1_lift}
                                    onChange={(e) => updateZp10s("servo1_lift", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>

                            <div>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机2 抬起角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{zp10s.servo2_lift}°</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="270"
                                    value={zp10s.servo2_lift}
                                    onChange={(e) => updateZp10s("servo2_lift", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>
                        </div>
                    </div>
                </div>
            )}

            {/* STS3215 Config */}
            {driver === "sts3215" && (
                <div style={{padding: 16}}>
                    <div
                        style={{
                            background: "#fff",
                            borderRadius: 10,
                            padding: 16,
                            boxShadow: "0 1px 2px rgba(0,0,0,0.05)",
                        }}
                    >
                        <h3 style={{fontSize: 15, fontWeight: 600, marginBottom: 16}}>STS3215 抓取序列</h3>

                        <div style={{marginBottom: 20}}>
                            <div style={{fontSize: 13, fontWeight: 600, color: "#8e8e93", marginBottom: 12}}>准备阶段</div>
                            <div style={{marginBottom: 16}}>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机1 准备角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{sts.servo1_prepare}</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="4095"
                                    value={sts.servo1_prepare}
                                    onChange={(e) => updateSts("servo1_prepare", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>

                            <div style={{marginBottom: 16}}>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机2 准备角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{sts.servo2_prepare}</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="4095"
                                    value={sts.servo2_prepare}
                                    onChange={(e) => updateSts("servo2_prepare", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>

                            <div>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机3 准备角度 (张开)</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{sts.servo3_prepare}</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="4095"
                                    value={sts.servo3_prepare}
                                    onChange={(e) => updateSts("servo3_prepare", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>
                        </div>

                        <div style={{marginBottom: 20}}>
                            <div style={{fontSize: 13, fontWeight: 600, color: "#8e8e93", marginBottom: 12}}>进入阶段</div>
                            <div style={{marginBottom: 16}}>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机1 进入角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{sts.servo1_enter}</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="4095"
                                    value={sts.servo1_enter}
                                    onChange={(e) => updateSts("servo1_enter", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>

                            <div style={{marginBottom: 16}}>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机2 进入角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{sts.servo2_enter}</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="4095"
                                    value={sts.servo2_enter}
                                    onChange={(e) => updateSts("servo2_enter", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>

                            <div>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机3 进入角度 (保持张开)</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{sts.servo3_enter}</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="4095"
                                    value={sts.servo3_enter}
                                    onChange={(e) => updateSts("servo3_enter", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>
                        </div>

                        <div style={{marginBottom: 20}}>
                            <div style={{fontSize: 13, fontWeight: 600, color: "#8e8e93", marginBottom: 12}}>抓取阶段</div>
                            <div>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机3 抓取角度 (闭合)</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{sts.servo3_grab}</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="4095"
                                    value={sts.servo3_grab}
                                    onChange={(e) => updateSts("servo3_grab", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>
                        </div>

                        <div>
                            <div style={{fontSize: 13, fontWeight: 600, color: "#8e8e93", marginBottom: 12}}>抬起阶段</div>
                            <div style={{marginBottom: 16}}>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机1 抬起角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{sts.servo1_lift}</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="4095"
                                    value={sts.servo1_lift}
                                    onChange={(e) => updateSts("servo1_lift", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>

                            <div style={{marginBottom: 16}}>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机2 抬起角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{sts.servo2_lift}</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="4095"
                                    value={sts.servo2_lift}
                                    onChange={(e) => updateSts("servo2_lift", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>

                            <div>
                                <div style={{display: "flex", justifyContent: "space-between", marginBottom: 8}}>
                                    <span style={{fontSize: 14}}>舵机3 抬起角度</span>
                                    <span style={{fontSize: 14, fontWeight: 600, color: "#007aff"}}>{sts.servo3_lift}</span>
                                </div>
                                <input
                                    type="range"
                                    min="0"
                                    max="4095"
                                    value={sts.servo3_lift}
                                    onChange={(e) => updateSts("servo3_lift", parseInt(e.target.value))}
                                    style={{width: "100%"}}
                                />
                            </div>
                        </div>
                    </div>
                </div>
            )}

            <div style={{padding: 16, paddingTop: 0}}>
                <div
                    style={{
                        background: "#fff",
                        borderRadius: 10,
                        padding: 16,
                        boxShadow: "0 1px 2px rgba(0,0,0,0.05)",
                    }}
                >
                    <div style={{display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 16}}>
                        <div>
                            <h3 style={{fontSize: 15, fontWeight: 600, margin: 0}}>底盘 PWM 通道</h3>
                            <div style={{fontSize: 12, color: "#8e8e93", marginTop: 4}}>
                                配置文件保存到 `base_pwm_channels.json`
                            </div>
                        </div>
                        <button
                            onClick={savePwmChannels}
                            disabled={savingPwm}
                            style={{
                                color: savingPwm ? "#8e8e93" : "#007aff",
                                fontSize: 15,
                                cursor: savingPwm ? "not-allowed" : "pointer",
                                background: "none",
                                border: "none",
                                fontWeight: 600,
                            }}
                        >
                            {savingPwm ? "保存中..." : "保存 PWM"}
                        </button>
                    </div>

                    {pwmStatus && (
                        <div
                            style={{
                                textAlign: "center",
                                padding: 10,
                                marginBottom: 16,
                                borderRadius: 8,
                                background: pwmStatus.includes("成功") || pwmStatus.includes("生效") ? "#d4edda" : "#f8d7da",
                                color: pwmStatus.includes("成功") || pwmStatus.includes("生效") ? "#155724" : "#721c24",
                            }}
                        >
                            {pwmStatus}
                        </div>
                    )}

                    <div style={{display: "grid", gridTemplateColumns: "repeat(2, minmax(0, 1fr))", gap: 10, marginBottom: 16}}>
                        <button
                            onClick={swapLeftRightWheels}
                            style={{padding: "10px 12px", borderRadius: 8, border: "1px solid #d1d1d6", background: "#f7f7fa", cursor: "pointer", fontSize: 14}}
                        >
                            切换左右轮
                        </button>
                        <button
                            onClick={swapLeftWheelDirection}
                            style={{padding: "10px 12px", borderRadius: 8, border: "1px solid #d1d1d6", background: "#f7f7fa", cursor: "pointer", fontSize: 14}}
                        >
                            左轮前后切换
                        </button>
                        <button
                            onClick={swapRightWheelDirection}
                            style={{padding: "10px 12px", borderRadius: 8, border: "1px solid #d1d1d6", background: "#f7f7fa", cursor: "pointer", fontSize: 14}}
                        >
                            右轮前后切换
                        </button>
                    </div>

                    {[
                        {key: "left_ch1" as const, label: "左轮前进 ch1"},
                        {key: "left_ch2" as const, label: "左轮后退 ch2"},
                        {key: "right_ch1" as const, label: "右轮前进 ch1"},
                        {key: "right_ch2" as const, label: "右轮后退 ch2"},
                    ].map((item) => (
                        <div
                            key={item.key}
                            style={{
                                display: "flex",
                                justifyContent: "space-between",
                                alignItems: "center",
                                padding: "12px 0",
                                borderTop: "1px solid #f2f2f7",
                                gap: 12,
                            }}
                        >
                            <span style={{fontSize: 14}}>{item.label}</span>
                            <input
                                type="number"
                                min="0"
                                step="1"
                                value={basePwmChannels[item.key]}
                                onChange={(e) => updateBasePwmChannels(item.key, parseInt(e.target.value || "0", 10))}
                                style={{
                                    width: 88,
                                    border: "1px solid #d1d1d6",
                                    borderRadius: 8,
                                    padding: "8px 10px",
                                    fontSize: 14,
                                }}
                            />
                        </div>
                    ))}
                </div>
            </div>

            {/* Back button */}
            <div style={{padding: "0 16px 30px"}}>
                <button
                    onClick={() => window.history.back()}
                    style={{
                        width: "100%",
                        padding: 14,
                        border: "none",
                        borderRadius: 10,
                        background: "#e5e5ea",
                        color: "#1c1c1e",
                        fontSize: 16,
                        fontWeight: 600,
                        cursor: "pointer",
                    }}
                >
                    返回
                </button>
            </div>
        </div>
    );
};

export default ArmAnglesPage;
