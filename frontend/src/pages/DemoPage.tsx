import React, {useState, useRef, useEffect, useCallback} from "react";
import {api} from "../api";
import ControlButton from "../components/ControlButton.tsx";
import Page from "../components/Page";
import Card from "../components/Card";
import ConfirmDialog from "../components/ConfirmDialog";
import {S} from "../styles";
import {useViewportScale} from "../hooks/useViewportScale";

// 与后端 /api/demo/list 对齐。**一张卡片 = 动作 × 模型**：
// 动作是预定义的通用脚本（grab / approach …，与模型无关），模型是 demo/models 里的一颗；
// 卡片由用户在这里新建，名字随便起（中文也行），参数存在卡片的配置里。
interface DemoInfo {
    name: string;       // 卡片名（用户起的）
    action: string;     // 动作脚本名（demo/<action>.lua）
    model: string;      // 模型名（demo/models/<model>.cvimodel）
    ready: boolean;     // 动作脚本与模型文件都在
    error?: string;     // 缺什么（缺了也照样列出来，点开始会报错）
}
// 动作清单：name 是脚本第一行 `-- name: 接近瞄准` 给的显示名，缺省就是文件名
interface ActionInfo { id: string; name: string; }

// 执行方式：once = 跑一遍就结束；loop = 跑完接着跑，直到你按停止（**没有超时**）
type RunMode = "once" | "loop";
interface DemoParams { target_size: number; speed: number; turn_speed: number; mode: RunMode; }
// 表单里数字存字符串：编辑期间不解析，清空/输一半都不会突然跳成 0；保存时才转数字。
type DemoForm = { target_size: string; speed: string; turn_speed: string; mode: RunMode };

const DEFAULT_PARAMS: DemoParams = {target_size: 300, speed: 25, turn_speed: 25, mode: "once"};
const EMPTY_FORM: DemoForm = {target_size: "", speed: "", turn_speed: "", mode: "once"};
const MODES: {id: RunMode; label: string}[] = [
    {id: "once", label: "执行一次"},
    {id: "loop", label: "循环执行"},
];
const NUM_FIELDS: {key: "target_size" | "speed" | "turn_speed"; label: string; unit: string}[] = [
    {key: "target_size", label: "目标框宽", unit: "px"},
    {key: "speed", label: "直线速度", unit: "%"},
    {key: "turn_speed", label: "转弯速度", unit: "%"},
];

const formOf = (p: DemoParams): DemoForm => ({
    target_size: String(p.target_size), speed: String(p.speed), turn_speed: String(p.turn_speed),
    mode: p.mode === "loop" ? "loop" : "once",
});
// 空着的字段回落到默认值（与后端 /api/demo/config 的默认值一致）
const parseForm = (f: DemoForm): DemoParams => ({
    target_size: parseInt(f.target_size, 10) || DEFAULT_PARAMS.target_size,
    speed: parseInt(f.speed, 10) || DEFAULT_PARAMS.speed,
    turn_speed: parseInt(f.turn_speed, 10) || DEFAULT_PARAMS.turn_speed,
    mode: f.mode === "loop" ? "loop" : "once",
});

const DemoPage = () => {
    const {scalePx} = useViewportScale();
    const [demos, setDemos] = useState<DemoInfo[]>([]);
    const [actions, setActions] = useState<ActionInfo[]>([]);
    const [models, setModels] = useState<string[]>([]);
    const [demoStatus, setDemoStatus] = useState("准备就绪");
    const [runningDemo, setRunningDemo] = useState<string | null>(null);
    const [demoLoading, setDemoLoading] = useState(false);
    const runningDemoRef = useRef<string | null>(null);

    // 每张卡片一份参数（键 = 卡片名）；展开/收起是每张卡自己的状态
    const [params, setParams] = useState<Record<string, DemoForm>>({});
    const [expanded, setExpanded] = useState<Record<string, boolean>>({});
    const [savedHint, setSavedHint] = useState<string | null>(null);
    const [savingName, setSavingName] = useState<string | null>(null);

    // 新建卡片
    const [showCreate, setShowCreate] = useState(false);
    const [newName, setNewName] = useState("");
    const [newAction, setNewAction] = useState("");
    const [newModel, setNewModel] = useState("");
    const [newForm, setNewForm] = useState<DemoForm>(EMPTY_FORM);
    const [creating, setCreating] = useState(false);
    const [deleteTarget, setDeleteTarget] = useState<string | null>(null);

    const fetchDemoList = useCallback(() => {
        api.demo.list().then(data => {
            const list: DemoInfo[] = data.demos || [];
            setDemos(list);
            const acts: ActionInfo[] = data.actions || [];
            setActions(acts);
            const mods: string[] = data.models || [];
            setModels(mods);
            // 新建表单预选第一项，省得点了"创建"才被告知"选一个动作/模型"
            setNewAction(prev => prev || acts[0]?.id || "");
            setNewModel(prev => prev || mods[0] || "");
            // 顺带把每张卡片存下来的参数拉回来
            list.forEach(d => {
                api.demo.getConfig(d.name).then((p: Partial<DemoParams>) => {
                    setParams(prev => ({
                        ...prev,
                        [d.name]: formOf({
                            target_size: p.target_size ?? DEFAULT_PARAMS.target_size,
                            speed: p.speed ?? DEFAULT_PARAMS.speed,
                            turn_speed: p.turn_speed ?? DEFAULT_PARAMS.turn_speed,
                            mode: p.mode === "loop" ? "loop" : "once",
                        }),
                    }));
                }).catch(() => {});
            });
        }).catch(() => {});
    }, []);

    useEffect(() => { fetchDemoList(); }, [fetchDemoList]);

    // 运行状态**以后端为准**（原来是纯本地 state）：轮询 /api/demo/status，
    // 这样切到别的页面再回来、刷新浏览器、甚至脚本是别的客户端起的，都能正确显示
    // "哪张卡片在跑"并给出停止按钮。
    useEffect(() => {
        let alive = true;
        const tick = () => {
            api.demo.status().then((st: {state?: string; card?: string; script?: string}) => {
                if (!alive) return;
                const running = st?.state === "running" ? (st.card || st.script || "") : null;
                setRunningDemo(running);
                runningDemoRef.current = running;
            }).catch(() => {});
        };
        tick();
        const id = setInterval(tick, 2000);
        return () => { alive = false; clearInterval(id); };
    }, []);

    const runDemo = async (name: string) => {
        if (runningDemoRef.current !== null) {
            setDemoLoading(true);
            try { await api.demo.stop(); } catch {}
            setDemoLoading(false);
            setDemoStatus(`${runningDemoRef.current} 已停止`);
            setRunningDemo(null);
            runningDemoRef.current = null;
            if (runningDemo === name) return;
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

    const changeParam = (name: string, key: "target_size" | "speed" | "turn_speed", value: string) => {
        const digits = value.replace(/[^0-9]/g, "");
        setParams(prev => ({...prev, [name]: {...(prev[name] || formOf(DEFAULT_PARAMS)), [key]: digits}}));
    };

    const saveParams = async (name: string, card: DemoInfo) => {
        const p = parseForm(params[name] || formOf(DEFAULT_PARAMS));
        setParams(prev => ({...prev, [name]: formOf(p)}));
        setSavingName(name);
        try {
            const r = await api.demo.setConfig(name, {action: card.action, model: card.model, ...p});
            setSavedHint(r.error
                ? `${name}: 保存失败 ${r.error}`
                : `${name}: 已保存（${p.mode === "loop" ? "循环执行" : "执行一次"}，框宽 ${p.target_size}px，直线 ${p.speed}%）`);
        } catch (err) { setSavedHint(`${name}: 保存失败 ${err}`); }
        finally { setSavingName(null); setTimeout(() => setSavedHint(null), 4000); }
    };

    const createCard = async () => {
        const name = newName.trim();
        const bad = !name ? "先给这张卡片起个名字" : !newAction ? "选一个动作" : !newModel ? "选一个模型" : "";
        if (bad) { setSavedHint(bad); setTimeout(() => setSavedHint(null), 4000); return; }
        setCreating(true);
        try {
            const r = await api.demo.setConfig(name, {action: newAction, model: newModel, ...parseForm(newForm)});
            if (r.error) {
                setSavedHint(`新建失败 ${r.error}`);
            } else {
                setSavedHint(`已新建：${name}`);
                setShowCreate(false);
                setNewName("");
                setNewForm(EMPTY_FORM);
                fetchDemoList();
            }
        } catch (err) { setSavedHint(`新建失败 ${err}`); }
        finally { setCreating(false); setTimeout(() => setSavedHint(null), 4000); }
    };

    const doDelete = async () => {
        const name = deleteTarget;
        setDeleteTarget(null);
        if (!name) return;
        try {
            const r = await api.demo.remove(name);
            setSavedHint(r.error ? `删除失败 ${r.error}` : `已删除：${name}`);
        } catch (err) { setSavedHint(`删除失败 ${err}`); }
        finally { fetchDemoList(); setTimeout(() => setSavedHint(null), 4000); }
    };

    const maxW = {width: "100%", maxWidth: scalePx(420)};
    // 状态点：空闲=低调的灰，运行=绿+光晕。（不要用 S.dot —— 它不运行时是红的，
    // 一屏卡片全是红点，看着像每一张都出错了）
    const statusDot = (on: boolean): React.CSSProperties => ({
        width: scalePx(6), height: scalePx(6), borderRadius: "50%", flexShrink: 0,
        background: on ? "var(--color-success)" : "var(--color-bg-elevated)",
        boxShadow: on ? "0 0 8px rgba(34,197,94,0.5)" : "none",
        transition: "all 0.3s",
    });
    const labelStyle = {fontSize: scalePx(10), color: "var(--color-text-dim)"};
    const inputStyle = {
        width: "100%", boxSizing: "border-box" as const,
        padding: `${scalePx(7)} ${scalePx(6)}`,
        fontSize: scalePx(14), textAlign: "center" as const,
        background: "var(--color-bg-elevated)", color: "var(--color-text)",
        border: "1px solid var(--color-border-light)", borderRadius: scalePx(6),
    };

    /** 小标签：执行方式用它就够了，比两个大按钮安静得多 */
    const badge = (text: string, tone: RunMode) => (
        <span style={{
            fontSize: scalePx(9), fontWeight: 700, letterSpacing: "0.5px",
            padding: `${scalePx(1.5)} ${scalePx(6)}`, borderRadius: scalePx(4),
            background: tone === "loop" ? "var(--color-primary-soft)" : "var(--color-bg-subtle)",
            color: tone === "loop" ? "var(--color-primary)" : "var(--color-text-muted)",
        }}>{text}</span>
    );

    /** 二选一：一个控件里两半（选中那半填主色） */
    const segment = (value: RunMode, onChange: (m: RunMode) => void) => (
        <div style={{
            display: "inline-flex", padding: scalePx(2), gap: scalePx(2),
            background: "var(--color-bg-subtle)", borderRadius: scalePx(8),
        }}>
            {MODES.map(o => (
                <button key={o.id} onClick={() => onChange(o.id)} style={{
                    padding: `${scalePx(5)} ${scalePx(10)}`, border: "none", cursor: "pointer",
                    borderRadius: scalePx(6), fontSize: scalePx(11), fontWeight: 600,
                    background: value === o.id ? "var(--color-primary)" : "transparent",
                    color: value === o.id ? "#fff" : "var(--color-text-muted)",
                }}>{o.label}</button>
            ))}
        </div>
    );

    /** 多选一（动作 / 模型）：一排可点的小标签 */
    const chooser = (options: {id: string; label: string}[], selected: string, onPick: (id: string) => void) => (
        <div style={{display: "flex", gap: scalePx(6), flexWrap: "wrap"}}>
            {options.length === 0 ? (
                <span style={{fontSize: scalePx(11), color: "var(--color-text-dim)"}}>（没有可选项）</span>
            ) : options.map(o => (
                <button key={o.id} onClick={() => onPick(o.id)} style={{
                    padding: `${scalePx(5)} ${scalePx(11)}`, cursor: "pointer",
                    fontSize: scalePx(12), borderRadius: scalePx(6), border: "none",
                    background: selected === o.id ? "var(--color-primary)" : "var(--color-bg-subtle)",
                    color: selected === o.id ? "#fff" : "var(--color-text)",
                }}>{o.label}</button>
            ))}
        </div>
    );

    /** 数字输入：标签在上、单位贴在框内右侧 */
    const numField = (
        form: DemoForm,
        onChange: (key: "target_size" | "speed" | "turn_speed", v: string) => void,
        f: {key: "target_size" | "speed" | "turn_speed"; label: string; unit: string},
    ) => (
        <div key={f.key} style={{display: "flex", flexDirection: "column", gap: scalePx(3), flex: "1 1 0", minWidth: scalePx(70)}}>
            <span style={labelStyle}>{f.label}</span>
            <div style={{position: "relative"}}>
                <input
                    type="text" inputMode="numeric" pattern="[0-9]*"
                    value={form[f.key]} placeholder={String(DEFAULT_PARAMS[f.key])}
                    onChange={e => onChange(f.key, e.target.value)}
                    style={inputStyle}
                />
                <span style={{
                    position: "absolute", right: scalePx(6), top: "50%", transform: "translateY(-50%)",
                    fontSize: scalePx(9), color: "var(--color-text-dim)", pointerEvents: "none",
                }}>{f.unit}</span>
            </div>
        </div>
    );

    const actionLabel = (id: string) => actions.find(a => a.id === id)?.name || id;
    const formOfCard = (name: string) => params[name] || formOf(DEFAULT_PARAMS);

    return (
        <Page center>
            <h2 style={{fontSize: scalePx(17), fontWeight: 700, marginBottom: scalePx(2), marginTop: "20px"}}>Demo 控制台</h2>

            <div style={{...maxW, marginTop: scalePx(14)}}>
                <div style={{...S.rowBetween, marginBottom: scalePx(8)}}>
                    <h3 style={{fontSize: scalePx(14), fontWeight: 600, margin: 0}}>本地 Demo</h3>
                    <ControlButton variant={showCreate ? "secondary" : "primary"} size="small"
                                   onClick={() => setShowCreate(v => !v)}>
                        {showCreate ? "取消" : "新建"}
                    </ControlButton>
                </div>

                {/* 新建：动作 × 模型 + 参数 */}
                {showCreate && (
                    <Card marginBottom={10}>
                        <div style={{display: "flex", flexDirection: "column", gap: scalePx(12)}}>
                            <div style={{display: "flex", flexDirection: "column", gap: scalePx(4)}}>
                                <span style={labelStyle}>名称</span>
                                <input
                                    type="text" placeholder="例如：追网球接近"
                                    value={newName} onChange={e => setNewName(e.target.value)}
                                    style={{...inputStyle, textAlign: "left"}}
                                />
                            </div>
                            <div style={{display: "flex", flexDirection: "column", gap: scalePx(4)}}>
                                <span style={labelStyle}>动作（做什么）</span>
                                {chooser(actions.map(a => ({id: a.id, label: a.name})), newAction, setNewAction)}
                            </div>
                            <div style={{display: "flex", flexDirection: "column", gap: scalePx(4)}}>
                                <span style={labelStyle}>模型（找什么）</span>
                                {chooser(models.map(m => ({id: m, label: m})), newModel, setNewModel)}
                            </div>
                            <div style={{display: "flex", alignItems: "flex-end", gap: scalePx(8), flexWrap: "wrap"}}>
                                {NUM_FIELDS.map(f => numField(newForm, (k, v) =>
                                    setNewForm(prev => ({...prev, [k]: v.replace(/[^0-9]/g, "")})), f))}
                            </div>
                            <div style={{...S.rowBetween}}>
                                <span style={labelStyle}>执行方式</span>
                                {segment(newForm.mode, m => setNewForm(prev => ({...prev, mode: m})))}
                            </div>
                            <div style={{display: "flex", justifyContent: "flex-end"}}>
                                <ControlButton variant="primary" size="small" onClick={createCard} loading={creating}>
                                    创建
                                </ControlButton>
                            </div>
                        </div>
                    </Card>
                )}

                {demos.length === 0 ? (
                    <Card>
                        <div style={{textAlign: "center", padding: scalePx(20), color: "var(--color-text-muted)", fontSize: scalePx(12), lineHeight: 1.7}}>
                            还没有 Demo<br />
                            <span style={{color: "var(--color-text-dim)"}}>点上面的「新建」，挑一个动作 + 一个模型</span>
                        </div>
                    </Card>
                ) : (
                    <div style={{display: "flex", flexDirection: "column", gap: scalePx(8)}}>
                        {demos.map(card => {
                            const name = card.name;
                            const isRunning = runningDemo === name;
                            const form = formOfCard(name);
                            const open = !!expanded[name];
                            return (
                                <Card key={name} marginBottom={0} style={{
                                    border: isRunning ? "1px solid var(--color-success)" : "1px solid transparent",
                                    transition: "border-color 0.2s",
                                }}>
                                    {/* 第一行：谁在跑 / 干什么 / 开关 */}
                                    <div style={{...S.rowBetween, alignItems: "flex-start", gap: scalePx(8)}}>
                                        <div style={{minWidth: 0}}>
                                            <div style={{...S.row, gap: scalePx(6)}}>
                                                <span style={statusDot(isRunning)} />
                                                <span style={{
                                                    fontSize: scalePx(14), fontWeight: 600,
                                                    overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap",
                                                }}>{name}</span>
                                            </div>
                                            <div style={{...S.row, gap: scalePx(5), marginTop: scalePx(3), flexWrap: "wrap"}}>
                                                {badge(form.mode === "loop" ? "循环" : "一次", form.mode)}
                                                <span style={{fontSize: scalePx(11), color: "var(--color-text-dim)"}}>
                                                    {actionLabel(card.action)} · {card.model}
                                                </span>
                                            </div>
                                            {!card.ready && (
                                                <div style={{fontSize: scalePx(11), color: "var(--color-danger)", marginTop: scalePx(3)}}>
                                                    {card.error || "配置不完整"}
                                                </div>
                                            )}
                                        </div>
                                        <div style={{...S.row, gap: scalePx(4), flexShrink: 0}}>
                                            <ControlButton
                                                variant={isRunning ? "danger" : "primary"}
                                                size="small"
                                                onClick={() => runDemo(name)}
                                                disabled={demoLoading && !isRunning}
                                                loading={demoLoading && isRunning}
                                            >
                                                {isRunning ? "停止" : "启动"}
                                            </ControlButton>
                                            <button onClick={() => setDeleteTarget(name)} title="删除这张卡片" style={{
                                                background: "none", border: "none", cursor: "pointer",
                                                padding: scalePx(6), borderRadius: scalePx(6),
                                                color: "var(--color-text-dim)", fontSize: scalePx(11),
                                            }}>删除</button>
                                        </div>
                                    </div>

                                    {/* 运行参数：默认收起（平时只按启动），点开才编辑 */}
                                    <div style={{marginTop: scalePx(10), paddingTop: scalePx(10), borderTop: "1px solid var(--color-border-light)"}}>
                                        <div style={{...S.rowBetween, cursor: "pointer"}} onClick={() => setExpanded(p => ({...p, [name]: !open}))}>
                                            <span style={{fontSize: scalePx(10), color: "var(--color-text-dim)", letterSpacing: "1px"}}>运行参数</span>
                                            <span style={{fontSize: scalePx(11), color: "var(--color-text-dim)"}}>
                                                {open ? "收起" : `框宽 ${form.target_size}px · 直线 ${form.speed}%`}
                                            </span>
                                        </div>
                                        {open && (
                                            <div style={{display: "flex", flexDirection: "column", gap: scalePx(10), marginTop: scalePx(10)}}>
                                                <div style={{display: "flex", alignItems: "flex-end", gap: scalePx(8), flexWrap: "wrap"}}>
                                                    {NUM_FIELDS.map(f => numField(form, (k, v) => changeParam(name, k, v), f))}
                                                </div>
                                                <div style={{...S.rowBetween}}>
                                                    <span style={labelStyle}>执行方式</span>
                                                    {segment(form.mode, m => setParams(prev => ({
                                                        ...prev, [name]: {...formOfCard(name), mode: m},
                                                    })))}
                                                </div>
                                                <div style={{display: "flex", justifyContent: "flex-end"}}>
                                                    <ControlButton variant="secondary" size="small"
                                                                   onClick={() => saveParams(name, card)}
                                                                   loading={savingName === name}>
                                                        保存
                                                    </ControlButton>
                                                </div>
                                            </div>
                                        )}
                                    </div>
                                </Card>
                            );
                        })}
                    </div>
                )}
            </div>

            {demoStatus && demoStatus !== "准备就绪" && (
                <div style={{
                    ...maxW, marginTop: scalePx(6), textAlign: "center", fontSize: scalePx(11),
                    color: demoStatus.includes("错误") ? "var(--color-danger)" : "var(--color-text-dim)",
                }}>
                    {demoStatus}
                </div>
            )}
            {savedHint && (
                <div style={{
                    ...maxW, marginTop: scalePx(4), textAlign: "center", fontSize: scalePx(11),
                    color: savedHint.includes("失败") || savedHint.includes("先") ? "var(--color-danger)" : "var(--color-success)",
                }}>
                    {savedHint}
                </div>
            )}

            <ConfirmDialog
                open={deleteTarget !== null}
                title="删除 Demo"
                message={`删除「${deleteTarget ?? ""}」？这张卡片的参数会一起删掉（动作脚本和模型文件不受影响）。`}
                confirmText="删除"
                danger
                onConfirm={doDelete}
                onCancel={() => setDeleteTarget(null)}
            />
        </Page>
    );
};

export default DemoPage;
