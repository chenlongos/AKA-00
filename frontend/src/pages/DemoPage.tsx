import {useState, useRef, useEffect, useCallback} from "react";
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

interface DemoParams { target_size: number; speed: number; turn_speed: number; max_seconds: number; }
// 表单里存字符串：编辑期间不解析，清空/输一半都不会突然跳成 0；保存时才转数字。
type DemoForm = { target_size: string; speed: string; turn_speed: string; max_seconds: string };

const DEFAULT_PARAMS: DemoParams = {target_size: 300, speed: 25, turn_speed: 25, max_seconds: 60};
const EMPTY_FORM: DemoForm = {target_size: "", speed: "", turn_speed: "", max_seconds: ""};
const formOf = (p: DemoParams): DemoForm => ({
    target_size: String(p.target_size), speed: String(p.speed), turn_speed: String(p.turn_speed),
    max_seconds: String(p.max_seconds),
});
// 空着的字段回落到默认值（与后端 /api/demo/config 的默认值一致）
const parseForm = (f: DemoForm): DemoParams => ({
    target_size: parseInt(f.target_size, 10) || DEFAULT_PARAMS.target_size,
    speed: parseInt(f.speed, 10) || DEFAULT_PARAMS.speed,
    turn_speed: parseInt(f.turn_speed, 10) || DEFAULT_PARAMS.turn_speed,
    max_seconds: parseInt(f.max_seconds, 10) || DEFAULT_PARAMS.max_seconds,
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

    // 每张卡片一份参数（键 = 卡片名）
    const [params, setParams] = useState<Record<string, DemoForm>>({});
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
            // 新建表单要的两份清单也跟着 list 一起回来
            const acts: ActionInfo[] = data.actions || [];
            setActions(acts);
            setModels(data.models || []);
            setNewAction(prev => prev || acts[0]?.id || "");
            // 顺带把每张卡片存下来的参数拉回来
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

    const saveParams = async (name: string, card: DemoInfo) => {
        const p = parseForm(params[name] || formOf(DEFAULT_PARAMS));
        setParams(prev => ({...prev, [name]: formOf(p)}));   // 回填解析后的值
        setSavingName(name);
        try {
            // 动作与模型要一起回传：卡片配置是整体覆盖写的
            const r = await api.demo.setConfig(name, {action: card.action, model: card.model, ...p});
            if (r.error) {
                setSavedHint(`${name}: 保存失败 ${r.error}`);
            } else {
                setSavedHint(`${name}: 已保存（目标框宽 ${p.target_size}px，直线 ${p.speed}%，转弯 ${p.turn_speed}%，超时 ${p.max_seconds}s）`);
            }
        } catch (err) { setSavedHint(`${name}: 保存失败 ${err}`); }
        finally { setSavingName(null); setTimeout(() => setSavedHint(null), 4000); }
    };

    const createCard = async () => {
        const name = newName.trim();
        if (!name) { setSavedHint("请先给这张卡片起个名字"); setTimeout(() => setSavedHint(null), 4000); return; }
        if (!newAction) { setSavedHint("请选一个动作"); setTimeout(() => setSavedHint(null), 4000); return; }
        if (!newModel) { setSavedHint("请选一个模型"); setTimeout(() => setSavedHint(null), 4000); return; }
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

    // 标签放上面、输入框占满整列 —— 比"输入框+后缀挤一行"能给足宽度
    const inputStyle = {
        width: "100%", boxSizing: "border-box" as const,
        padding: `${scalePx(7)} ${scalePx(6)}`,
        fontSize: scalePx(14), textAlign: "center" as const,
        background: "var(--color-bg-elevated)", color: "var(--color-text)",
        border: "1px solid var(--color-border-light)", borderRadius: scalePx(5),
    };
    const labelStyle = {fontSize: scalePx(10), color: "var(--color-text-dim)"};

    const numInput = (name: string, key: keyof DemoParams, label: string) => (
        <div style={{display: "flex", flexDirection: "column", gap: scalePx(3), flex: "1 1 0", minWidth: scalePx(72)}}>
            <span style={labelStyle}>{label}</span>
            <input
                type="text"
                inputMode="numeric"
                pattern="[0-9]*"
                value={(params[name] || formOf(DEFAULT_PARAMS))[key]}
                onChange={e => changeParam(name, key, e.target.value)}
                style={inputStyle}
            />
        </div>
    );

    // 新建表单里的数字输入（同一套样式，只是 key 在 newForm 上）
    const newNumInput = (key: keyof DemoParams, label: string) => (
        <div style={{display: "flex", flexDirection: "column", gap: scalePx(3), flex: "1 1 0", minWidth: scalePx(72)}}>
            <span style={labelStyle}>{label}</span>
            <input
                type="text"
                inputMode="numeric"
                pattern="[0-9]*"
                placeholder={String(DEFAULT_PARAMS[key])}
                value={newForm[key]}
                onChange={e => setNewForm(prev => ({...prev, [key]: e.target.value.replace(/[^0-9]/g, "")}))}
                style={inputStyle}
            />
        </div>
    );

    // 动作/模型的"下拉"：这个仓库里没有任何 <select>，用一排可选中的按钮代替
    const chooser = (
        options: {id: string; label: string}[],
        selected: string,
        onPick: (id: string) => void,
    ) => (
        <div style={{display: "flex", gap: scalePx(6), flexWrap: "wrap"}}>
            {options.length === 0 ? (
                <span style={{fontSize: scalePx(11), color: "var(--color-text-dim)"}}>（没有可选项）</span>
            ) : options.map(o => (
                <button
                    key={o.id}
                    onClick={() => onPick(o.id)}
                    style={{
                        padding: `${scalePx(5)} ${scalePx(12)}`,
                        fontSize: scalePx(12),
                        borderRadius: scalePx(6),
                        cursor: "pointer",
                        background: selected === o.id ? "var(--color-primary)" : "var(--color-bg-elevated)",
                        color: selected === o.id ? "#fff" : "var(--color-text)",
                        border: "1px solid var(--color-border-light)",
                    }}
                >
                    {o.label}
                </button>
            ))}
        </div>
    );

    const actionLabel = (id: string) => actions.find(a => a.id === id)?.name || id;

    return (
        <Page center>
            <h2 style={{fontSize: scalePx(17), fontWeight: 700, marginBottom: scalePx(2), marginTop: "20px"}}>Demo 控制台</h2>

            <div style={{...maxW, marginTop: scalePx(14)}}>
                <div style={{...S.rowBetween, marginBottom: scalePx(8)}}>
                    <h3 style={{fontSize: scalePx(14), fontWeight: 600, margin: 0}}>本地 Demo</h3>
                    <span style={{fontSize: scalePx(11), color: "var(--color-text-dim)"}}>
                        {runningDemo ? `运行中: ${runningDemo}` : `${demos.length} 个可用`}
                    </span>
                </div>

                {/* 新建：动作 × 模型 + 参数 */}
                <Card marginBottom={8}>
                    <div style={{...S.rowBetween}}>
                        <div style={{fontSize: scalePx(13), fontWeight: 600}}>新建 Demo</div>
                        <ControlButton variant="secondary" size="small" onClick={() => setShowCreate(v => !v)}>
                            {showCreate ? "收起" : "新建"}
                        </ControlButton>
                    </div>
                    {showCreate && (
                        <div style={{display: "flex", flexDirection: "column", gap: scalePx(10), marginTop: scalePx(10)}}>
                            <div style={{display: "flex", flexDirection: "column", gap: scalePx(3)}}>
                                <span style={labelStyle}>名称</span>
                                <input
                                    type="text"
                                    placeholder="例如：追网球接近"
                                    value={newName}
                                    onChange={e => setNewName(e.target.value)}
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
                            <div style={{display: "flex", alignItems: "center", gap: scalePx(8), flexWrap: "wrap"}}>
                                {newNumInput("target_size", "目标框宽 px")}
                                {newNumInput("speed", "直线速度 %")}
                                {newNumInput("turn_speed", "转弯速度 %")}
                                {newNumInput("max_seconds", "超时 s")}
                            </div>
                            <div style={{display: "flex", justifyContent: "flex-end"}}>
                                <ControlButton variant="primary" size="small" onClick={createCard} loading={creating}>
                                    创建
                                </ControlButton>
                            </div>
                        </div>
                    )}
                </Card>

                {demos.length === 0 ? (
                    <Card>
                        <div style={{textAlign: "center", padding: scalePx(24), color: "var(--color-text-muted)", fontSize: scalePx(13)}}>
                            还没有 Demo —— 上面「新建」挑一个动作 + 一个模型
                        </div>
                    </Card>
                ) : (
                    <div style={{display: "flex", flexDirection: "column", gap: scalePx(8)}}>
                        {demos.map(card => {
                            const name = card.name;
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
                                                <div style={{fontWeight: 600, fontSize: scalePx(14)}}>{name}</div>
                                                <div style={{fontSize: scalePx(11), color: "var(--color-text-dim)", marginTop: 1}}>
                                                    {actionLabel(card.action)} · {card.model}
                                                    {isRunning ? " · ● 运行中" : ""}
                                                </div>
                                                {!card.ready && (
                                                    <div style={{fontSize: scalePx(11), color: "var(--color-danger)", marginTop: 2}}>
                                                        {card.error || "配置不完整"}
                                                    </div>
                                                )}
                                            </div>
                                        </div>
                                        <div style={{display: "flex", gap: scalePx(6)}}>
                                            <ControlButton
                                                variant={isRunning ? "danger" : "success"}
                                                size="small"
                                                onClick={() => runDemo(name)}
                                                disabled={demoLoading && !isRunning}
                                                loading={demoLoading && isRunning}
                                            >
                                                {isRunning ? "停止" : "启动"}
                                            </ControlButton>
                                            <ControlButton variant="secondary" size="small" onClick={() => setDeleteTarget(name)}>
                                                删除
                                            </ControlButton>
                                        </div>
                                    </div>

                                    {/* 运行参数（跑这张卡片时传给动作脚本的 params）*/}
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
                                                    onClick={() => saveParams(name, card)}
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
