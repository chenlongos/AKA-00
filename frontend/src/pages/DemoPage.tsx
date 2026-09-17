import {useState, useRef, useEffect, useCallback} from "react";
import {api} from "../api";
import ControlButton from "../components/ControlButton.tsx";
import Page from "../components/Page";
import Card from "../components/Card";
import {S} from "../styles";
import {useViewportScale} from "../hooks/useViewportScale";

// 与后端 /api/demo/list 对齐：返回 {demos: DemoInfo[]}。这里只用 name。
interface DemoInfo { name: string; path: string; kind: string; }

// 跑 demo 时传给脚本的参数（每个 demo 各存一份，存在板上的 demo_config.json）。
// 这几个字段会作为 params 传给 scripts/chase.lua：target_size = 目标框宽（像素）、
// speed = 驱动速度百分比、max_seconds = 单次运行的总时长上限。
// speed = 直线速度，turn_speed = 转弯速度（分开：转弯要的占空比和直线不一样）
interface DemoParams { target_size: number; speed: number; turn_speed: number; max_seconds: number; }
// 表单里按键存字符串：编辑期间不解析，清空/输一半都不会突然跳成 0；保存时才转数字。
type DemoForm = { target_size: string; speed: string; turn_speed: string; max_seconds: string };

const DEFAULT_PARAMS: DemoParams = {target_size: 300, speed: 25, turn_speed: 25, max_seconds: 60};
const formOf = (p: DemoParams): DemoForm => ({
    target_size: String(p.target_size), speed: String(p.speed), turn_speed: String(p.turn_speed),
    max_seconds: String(p.max_seconds),
});

const DemoPage = () => {
    const {scalePx} = useViewportScale();
    const [demos, setDemos] = useState<DemoInfo[]>([]);
    const [demoStatus, setDemoStatus] = useState("准备就绪");
    const [runningDemo, setRunningDemo] = useState<string | null>(null);
    const [demoLoading, setDemoLoading] = useState(false);
    const runningDemoRef = useRef<string | null>(null);

    // 每个 demo 一份参数（键 = demo 名 = 模型名）
    const [params, setParams] = useState<Record<string, DemoForm>>({});
    const [savedHint, setSavedHint] = useState<string | null>(null);
    const [savingName, setSavingName] = useState<string | null>(null);

    const fetchDemoList = useCallback(() => {
        api.demo.list().then(data => {
            const list: DemoInfo[] = data.demos || [];
            setDemos(list);
            // 顺带把每个 demo 存下来的参数拉回来
            list.forEach(d => {
                api.demo.getConfig(d.name).then((p: Partial<DemoParams>) => {
                    setParams(prev => ({
                        ...prev,
                        [d.name]: formOf({
                            target_size: p.target_size ?? DEFAULT_PARAMS.target_size,
                            speed: p.speed ?? DEFAULT_PARAMS.speed,
                            turn_speed: p.turn_speed ?? DEFAULT_PARAMS.turn_speed,
                            max_seconds: p.max_seconds ?? DEFAULT_PARAMS.max_seconds,
                        }),
                    }));
                }).catch(() => {});
            });
        }).catch(() => {});
    }, []);

    useEffect(() => { fetchDemoList(); }, [fetchDemoList]);

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

    const changeParam = (name: string, key: keyof DemoParams, value: string) => {
        // 只留数字，避免用户输入法带进别的字符；不转成数字（见 DemoForm 的注释）
        const digits = value.replace(/[^0-9]/g, "");
        setParams(prev => ({...prev, [name]: {...(prev[name] || formOf(DEFAULT_PARAMS)), [key]: digits}}));
    };

    const saveParams = async (name: string) => {
        const form = params[name] || formOf(DEFAULT_PARAMS);
        const p: DemoParams = {
            target_size: parseInt(form.target_size, 10) || DEFAULT_PARAMS.target_size,
            speed: parseInt(form.speed, 10) || DEFAULT_PARAMS.speed,
            turn_speed: parseInt(form.turn_speed, 10) || DEFAULT_PARAMS.turn_speed,
            max_seconds: parseInt(form.max_seconds, 10) || DEFAULT_PARAMS.max_seconds,
        };
        setParams(prev => ({...prev, [name]: formOf(p)}));   // 回填解析后的值
        setSavingName(name);
        try {
            const r = await api.demo.setConfig(name, p);
            if (r.error) {
                setSavedHint(`${name}: 保存失败 ${r.error}`);
            } else {
                setSavedHint(`${name}: 已保存（目标框宽 ${p.target_size}px，直线 ${p.speed}%，转弯 ${p.turn_speed}%，超时 ${p.max_seconds}s）`);
            }
        } catch (err) { setSavedHint(`${name}: 保存失败 ${err}`); }
        finally { setSavingName(null); setTimeout(() => setSavedHint(null), 4000); }
    };

    const displayName = (name: string) => name.charAt(0).toUpperCase() + name.slice(1);
    const maxW = {width: "100%", maxWidth: scalePx(420)};

    // 标签放上面、输入框占满整列 —— 比"输入框+后缀挤一行"能给足宽度
    const numInput = (name: string, key: keyof DemoParams, label: string) => (
        <div style={{display: "flex", flexDirection: "column", gap: scalePx(3), flex: "1 1 0", minWidth: scalePx(72)}}>
            <span style={{fontSize: scalePx(10), color: "var(--color-text-dim)"}}>{label}</span>
            <input
                type="text"
                inputMode="numeric"
                pattern="[0-9]*"
                value={(params[name] || formOf(DEFAULT_PARAMS))[key]}
                onChange={e => changeParam(name, key, e.target.value)}
                style={{
                    width: "100%", boxSizing: "border-box",
                    padding: `${scalePx(7)} ${scalePx(6)}`,
                    fontSize: scalePx(14), textAlign: "center",
                    background: "var(--color-bg-elevated)", color: "var(--color-text)",
                    border: "1px solid var(--color-border-light)", borderRadius: scalePx(5),
                }}
            />
        </div>
    );

    return (
        <Page center>
            <h2 style={{fontSize: scalePx(17), fontWeight: 700, marginBottom: scalePx(2), marginTop: "20px"}}>Demo 控制台</h2>

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
                        {demos.map(({name}) => {
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

                                    {/* 运行参数（跑这个 demo 时传给脚本的 params）*/}
                                    <div style={{
                                        marginTop: scalePx(10), paddingTop: scalePx(10),
                                        borderTop: "1px solid var(--color-border-light)",
                                    }}>
                                        <div style={{
                                            fontSize: scalePx(10), color: "var(--color-text-dim)",
                                            textTransform: "uppercase", letterSpacing: "1px", marginBottom: scalePx(6),
                                        }}>
                                            运行参数
                                        </div>
                                        <div style={{display: "flex", alignItems: "center", gap: scalePx(8), flexWrap: "wrap"}}>
                                            {numInput(name, "target_size", "目标框宽 px")}
                                            {numInput(name, "speed", "直线速度 %")}
                                            {numInput(name, "turn_speed", "转弯速度 %")}
                                            {numInput(name, "max_seconds", "超时 s")}
                                            <div style={{display: "flex", alignItems: "flex-end"}}>
                                                <ControlButton
                                                    variant="secondary" size="small"
                                                    onClick={() => saveParams(name)}
                                                    loading={savingName === name}
                                                >
                                                    保存
                                                </ControlButton>
                                            </div>
                                        </div>
                                    </div>
                                </Card>
                            );
                        })}
                    </div>
                )}
            </div>

            {/* 状态行 */}
            {demoStatus && demoStatus !== "准备就绪" && (
                <div style={{
                    ...maxW, marginTop: scalePx(6), textAlign: "center",
                    fontSize: scalePx(11), color: demoStatus.includes("错误") ? "var(--color-danger)" : "var(--color-text-dim)",
                }}>
                    {demoStatus}
                </div>
            )}
            {savedHint && (
                <div style={{
                    ...maxW, marginTop: scalePx(4), textAlign: "center",
                    fontSize: scalePx(11), color: savedHint.includes("失败") ? "var(--color-danger)" : "var(--color-success)",
                }}>
                    {savedHint}
                </div>
            )}
        </Page>
    );
};

export default DemoPage;
