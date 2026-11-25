import {useEffect, useMemo, useRef, useState} from "react";
import type { CSSProperties } from "react";
import Header from "../components/Header";
import Card from "../components/Card";
import ConfirmDialog from "../components/ConfirmDialog";
import {useViewportScale} from "../hooks/useViewportScale";
import ItemList from "../selftest/components/ItemList.tsx";
import HistoryPanel from "../selftest/components/HistoryPanel.tsx";
import {createClient} from "../selftest/client.ts";
import {retestItem, runSelfTest} from "../selftest/runner.ts";
import type { ArmSnapshot } from "../selftest/runner.ts";
import {appendRecord, clearHistory, exportFilename, loadHistory} from "../selftest/history.ts";
import {
  DEMO_DEFAULTS,
  buildGroups,
  normalizeDemoParams,
  summarizeItems,
  type DemoParams,
  type ItemResult,
  type RunState,
  type SelfTestRecord,
} from "../selftest/core.ts";

const PAGE_STYLE = {
  minHeight: "100dvh",
  overflowY: "auto",
  "--color-bg": "#f2f2f7",
  "--color-bg-card": "#ffffff",
  "--color-bg-elevated": "#e5e5ea",
  "--color-text": "#1c1c1e",
  "--color-text-dim": "#8e8e93",
  "--color-text-muted": "#6e6e73",
  "--color-border": "#d1d1d6",
  "--color-border-light": "rgba(0,0,0,0.08)",
  "--color-primary": "#007aff",
  "--color-success": "#34c759",
  "--color-danger": "#ff3b30",
  color: "#1c1c1e",
  background: "#f2f2f7",
} as CSSProperties;

const DEMO_FIELDS: Array<{
  key: keyof DemoParams;
  label: string;
  unit: string;
  min: number;
  max: number;
  step: number;
}> = [
  {key: "speed", label: "直行速度", unit: "%", min: 5, max: 70, step: 1},
  {key: "turn_speed", label: "转弯速度", unit: "%", min: 5, max: 60, step: 1},
  {key: "target_size", label: "目标框宽", unit: "px", min: 50, max: 1000, step: 10},
];

const FLOW_ITEMS = [
  {icon: "🚗", title: "动作", desc: "前进/后退 10cm 与 2s，左右转向 30/60/90/180° 与 2s，抓取/放下"},
  {icon: "📷", title: "摄像头", desc: "打开摄像头、行内预览画面、关闭并确认"},
  {icon: "🎾", title: "自动抓网球（可选）", desc: "提示放球后启动视觉抓取脚本；可在选项里关闭"},
  {icon: "🦾", title: "舵机角度（可选）", desc: "底座 / 腕部 / 夹爪逐个大幅转动，最后恢复原始参数"},
  {icon: "📶", title: "WiFi（可选）", desc: "扫描并连接 Tsinghua-Dongsheng，不恢复原网络"},
];

const Toggle = ({on, disabled, onChange}: {
  on: boolean;
  disabled: boolean;
  onChange: (on: boolean) => void;
}) => {
  const {scalePx} = useViewportScale();
  return (
    <div
      role="switch"
      aria-checked={on}
      onClick={() => { if (!disabled) onChange(!on); }}
      style={{
        width: scalePx(42),
        height: scalePx(25),
        borderRadius: scalePx(13),
        background: on ? "var(--color-success)" : "#d1d1d6",
        position: "relative",
        flexShrink: 0,
        cursor: disabled ? "not-allowed" : "pointer",
        opacity: disabled ? 0.55 : 1,
        transition: "background 0.2s, opacity 0.15s",
      }}
    >
      <div style={{
        position: "absolute",
        top: scalePx(3),
        left: on ? scalePx(20) : scalePx(3),
        width: scalePx(19),
        height: scalePx(19),
        borderRadius: "50%",
        background: "#fff",
        boxShadow: "0 1px 3px rgba(0,0,0,0.3)",
        transition: "left 0.2s ease",
      }}/>
    </div>
  );
};

const OptionRow = ({icon, title, desc, on, disabled, onChange, accent}: {
  icon: string;
  title: string;
  desc: string;
  on: boolean;
  disabled: boolean;
  onChange: (on: boolean) => void;
  accent?: string;
}) => {
  const {scalePx} = useViewportScale();
  return (
    <div style={{display: "flex", alignItems: "center", gap: scalePx(10), padding: `${scalePx(7)} 0`}}>
      <span style={{
        display: "grid",
        placeItems: "center",
        width: scalePx(30),
        height: scalePx(30),
        borderRadius: scalePx(9),
        background: accent ?? "rgba(10,132,255,0.10)",
        fontSize: scalePx(15),
        flexShrink: 0,
      }}>{icon}</span>
      <div style={{flex: 1, minWidth: 0}}>
        <div style={{fontSize: scalePx(13), fontWeight: 600}}>{title}</div>
        <div style={{fontSize: scalePx(10), lineHeight: 1.45, color: "var(--color-text-muted)", marginTop: 1}}>{desc}</div>
      </div>
      <Toggle on={on} disabled={disabled} onChange={onChange}/>
    </div>
  );
};

const NoticeDialog = ({open, title, message, hint, confirmText, cancelText, onConfirm, onCancel}: {
  open: boolean;
  title: string;
  message: string;
  hint: string;
  confirmText: string;
  cancelText: string;
  onConfirm: () => void;
  onCancel: () => void;
}) => {
  const {scalePx} = useViewportScale();
  if (!open) return null;
  return (
    <div style={{
      position: "fixed", inset: 0, zIndex: 9999,
      display: "flex", alignItems: "center", justifyContent: "center",
      padding: scalePx(28),
    }}>
      <div style={{position: "absolute", inset: 0, background: "rgba(0,0,0,0.55)"}} onClick={onCancel}/>
      <div style={{
        position: "relative",
        width: "100%",
        maxWidth: scalePx(320),
        background: "var(--color-bg-card)",
        borderRadius: scalePx(18),
        padding: `${scalePx(20)} ${scalePx(18)} ${scalePx(14)}`,
        boxShadow: "0 24px 60px rgba(0,0,0,0.35)",
        animation: "fadeIn 0.2s ease-out",
      }}>
        <div style={{fontSize: scalePx(34), lineHeight: 1, textAlign: "center"}}>🎾</div>
        <div style={{fontSize: scalePx(16), fontWeight: 800, textAlign: "center", marginTop: scalePx(8)}}>{title}</div>
        <div style={{fontSize: scalePx(13), lineHeight: 1.65, color: "var(--color-text-muted)", textAlign: "center", marginTop: scalePx(6)}}>
          {message}
        </div>
        <div style={{
          marginTop: scalePx(10),
          padding: `${scalePx(8)} ${scalePx(10)}`,
          borderRadius: scalePx(10),
          background: "rgba(245,158,11,0.10)",
          border: "1px solid rgba(245,158,11,0.30)",
          fontSize: scalePx(11),
          lineHeight: 1.6,
          color: "#b45309",
        }}>{hint}</div>
        <button
          onClick={onConfirm}
          style={{
            width: "100%",
            marginTop: scalePx(14),
            padding: `${scalePx(12)} 0`,
            border: "none",
            borderRadius: scalePx(11),
            background: "linear-gradient(135deg, #0a84ff, #5e5ce6)",
            color: "#fff",
            fontSize: scalePx(15),
            fontWeight: 800,
            cursor: "pointer",
          }}
        >
          {confirmText}
        </button>
        <button
          onClick={onCancel}
          style={{
            width: "100%",
            marginTop: scalePx(8),
            padding: `${scalePx(10)} 0`,
            border: "none",
            borderRadius: scalePx(11),
            background: "var(--color-bg-elevated)",
            color: "var(--color-text-muted)",
            fontSize: scalePx(13),
            fontWeight: 600,
            cursor: "pointer",
          }}
        >
          {cancelText}
        </button>
      </div>
    </div>
  );
};

const SelfTestPage = () => {
  const {scalePx} = useViewportScale();
  const client = useMemo(() => createClient(), []);

  const [checked, setChecked] = useState(false);
  const [enableWifi, setEnableWifi] = useState(false);
  const [enableArm, setEnableArm] = useState(false);
  const [enableDemo, setEnableDemo] = useState(true);
  const [continueOnFailure, setContinueOnFailure] = useState(false);
  const [speed, setSpeed] = useState(25);
  const [demoForm, setDemoForm] = useState({
    speed: String(DEMO_DEFAULTS.speed),
    turn_speed: String(DEMO_DEFAULTS.turn_speed),
    target_size: String(DEMO_DEFAULTS.target_size),
  });
  const demoParams = useMemo(() => normalizeDemoParams({
    speed: Number(demoForm.speed),
    turn_speed: Number(demoForm.turn_speed),
    target_size: Number(demoForm.target_size),
  }), [demoForm]);
  const [retestId, setRetestId] = useState<string | null>(null);

  const [runState, setRunState] = useState<RunState>("idle");
  const [items, setItems] = useState<ItemResult[]>([]);
  const [precheckError, setPrecheckError] = useState("");
  const [info, setInfo] = useState({mac: "", ip: "", version: ""});

  const [confirmMessage, setConfirmMessage] = useState<string | null>(null);
  const [noticeMessage, setNoticeMessage] = useState<string | null>(null);
  const [cameraSrc, setCameraSrc] = useState("");
  const [armSnapshot, setArmSnapshot] = useState<ArmSnapshot | null>(null);

  const [remark, setRemark] = useState("");
  const [saved, setSaved] = useState(false);
  const [autoSaved, setAutoSaved] = useState(false);
  const [runMode, setRunMode] = useState<"manual" | "unattended">("manual");
  const [history, setHistory] = useState<SelfTestRecord[]>(() => loadHistory(window.localStorage));
  const [showClear, setShowClear] = useState(false);

  const abortRef = useRef(false);
  const confirmRef = useRef<((ok: boolean) => void) | null>(null);
  const noticeRef = useRef<((ok: boolean) => void) | null>(null);
  const frameWaiterRef = useRef<((ok: boolean) => void) | null>(null);

  const running = runState === "precheck" || runState === "running";

  useEffect(() => {
    let alive = true;
    Promise.all([client.systemInfo(), client.version()])
      .then(([system, version]) => {
        if (alive) setInfo({mac: system.mac, ip: system.ip, version});
      })
      .catch(() => {});
    return () => { alive = false; };
  }, [client]);

  const askConfirm = (_itemId: string, message: string) => new Promise<boolean>(resolve => {
    confirmRef.current = resolve;
    setConfirmMessage(message);
  });

  const answerConfirm = (ok: boolean) => {
    confirmRef.current?.(ok);
    confirmRef.current = null;
    setConfirmMessage(null);
  };

  const askNotice = (_itemId: string, message: string) => new Promise<boolean>(resolve => {
    noticeRef.current = resolve;
    setNoticeMessage(message);
  });

  const answerNotice = (ok: boolean) => {
    noticeRef.current?.(ok);
    noticeRef.current = null;
    setNoticeMessage(null);
  };

  const cameraFrame = (timeoutMs: number) => new Promise<boolean>(resolve => {
    let done = false;
    const finish = (ok: boolean) => {
      if (done) return;
      done = true;
      frameWaiterRef.current = null;
      if (!ok) setCameraSrc("");
      resolve(ok);
    };
    frameWaiterRef.current = finish;
    setCameraSrc(`/api/camera/stream?fps=10&t=${Date.now()}`);
    window.setTimeout(() => finish(false), timeoutMs);
  });

  const start = async () => {
    if (!checked || running) return;
    abortRef.current = false;
    setPrecheckError("");
    setSaved(false);
    setAutoSaved(false);
    setRunMode("manual");
    setRemark("");

    const groups = buildGroups({demo: enableDemo, arm: enableArm, wifi: enableWifi});

    const outcome = await runSelfTest({
      groups,
      speed,
      demoParams,
      abortOnFailure: !continueOnFailure,
      client,
      cameraFrame,
      confirm: askConfirm,
      notice: askNotice,
      isAborted: () => abortRef.current,
      onUpdate: (next, state) => {
        setItems(next);
        setRunState(state);
      },
      onArmSnapshot: setArmSnapshot,
      onCamera: on => {
        if (!on) setCameraSrc("");
      },
    });

    setItems(outcome.items);
    setPrecheckError(outcome.precheckError ?? "");
    setRunState(outcome.state);
  };

  const startUnattended = async () => {
    if (!checked || running) return;
    abortRef.current = false;
    setPrecheckError("");
    setSaved(false);
    setAutoSaved(false);
    setRemark("");
    setRunMode("unattended");

    const outcome = await runSelfTest({
      groups: buildGroups({demo: enableDemo, arm: enableArm, wifi: enableWifi}),
      speed,
      demoParams,
      abortOnFailure: false,
      unattended: true,
      client,
      cameraFrame,
      confirm: askConfirm,
      notice: askNotice,
      isAborted: () => abortRef.current,
      onUpdate: (next, state) => {
        setItems(next);
        setRunState(state);
      },
      onArmSnapshot: setArmSnapshot,
      onCamera: on => {
        if (!on) setCameraSrc("");
      },
    });

    setItems(outcome.items);
    setPrecheckError(outcome.precheckError ?? "");
    setRunState(outcome.state);

    const remarkText = outcome.precheckError
      ? "无人值守（预检失败）"
      : outcome.state === "aborted" ? "无人值守（中途中止）" : "无人值守";
    setRemark(remarkText);
    setHistory(appendRecord(
      window.localStorage,
      buildRecord(outcome.items, remarkText, "unattended"),
    ));
    setSaved(true);
    setAutoSaved(true);
  };

  const retest = async (id: string) => {
    if (running) return;
    abortRef.current = false;
    setPrecheckError("");
    setSaved(false);
    setAutoSaved(false);
    setRetestId(id);
    setRunState("running");
    try {
      const item = await retestItem(id, {
        speed,
        demoParams,
        abortOnFailure: !continueOnFailure,
        client,
        cameraFrame,
        confirm: askConfirm,
        notice: askNotice,
        isAborted: () => abortRef.current,
        onItem: next => setItems(prev => prev.map(entry => entry.id === id ? next : entry)),
        onArmSnapshot: setArmSnapshot,
        onCamera: on => {
          if (!on) setCameraSrc("");
        },
      });
      setItems(prev => prev.map(entry => entry.id === id ? item : entry));
    } finally {
      setRetestId(null);
      setRunState(abortRef.current ? "aborted" : "finished");
    }
  };

  const abort = () => {
    abortRef.current = true;
    answerConfirm(false);
    answerNotice(false);
    void client.stop();
    void client.demoStop();
  };

  const restoreArm = () => {
    if (!armSnapshot) return;
    void client.armWrite(armSnapshot.driver, armSnapshot.angles);
  };

  const buildRecord = (
    sourceItems: ItemResult[],
    remarkText: string,
    mode: "manual" | "unattended",
  ): SelfTestRecord => {
    const ts = Date.now();
    const sum = summarizeItems(sourceItems);
    return {
      id: `${ts}-${Math.random().toString(36).slice(2, 8)}`,
      ts,
      mac: info.mac,
      version: info.version,
      ip: info.ip,
      speed,
      demo: demoParams,
      items: sourceItems,
      summary: {
        pass: sum.pass,
        fail: sum.fail,
        skip: sum.skip,
        unconfirmed: sum.unconfirmed,
      },
      remark: remarkText,
      mode,
    };
  };

  const saveRecord = () => {
    if (items.length === 0) return;
    const record = buildRecord(items, remark.trim(), runMode);
    setHistory(appendRecord(window.localStorage, record));
    setSaved(true);
  };

  const exportRecord = (record: SelfTestRecord) => {
    const blob = new Blob([JSON.stringify(record, null, 2)], {type: "application/json"});
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = exportFilename(record.mac, record.ts);
    link.click();
    URL.revokeObjectURL(url);
  };

  const clearAll = () => {
    clearHistory(window.localStorage);
    setHistory([]);
    setShowClear(false);
  };

  const summary = summarizeItems(items);
  const done = summary.pass + summary.fail + summary.skip;
  const percent = items.length > 0 ? Math.round((done / items.length) * 100) : 0;
  const activeItem = items.find(item => item.status === "running" || item.status === "confirm") ?? null;
  const canSave = (runState === "finished" || runState === "aborted") && items.length > 0 && !saved;

  const heroTitle =
    runState === "precheck" ? "正在预检…"
      : runState === "running" ? (activeItem ? activeItem.name : "测试进行中…")
        : runState === "finished" ? (summary.ok ? "全部通过" : "存在失败项")
          : runState === "aborted" ? "测试已中止"
            : "准备就绪";

  const heroSubtitle =
    runState === "idle" ? "勾选安全确认后选择「开始测试」（摄像头/抓球人工确认）或「全自动测试」（全接口判定、自动保存）"
      : runState === "precheck" ? "检查底盘连接与网球模型"
        : runState === "running"
          ? `已完成 ${done} / ${items.length} 项 · ${activeItem?.status === "confirm" ? "等待人工确认" : "正在执行"}`
          : `已完成 ${done} / ${items.length} 项 · 通过 ${summary.pass} · 失败 ${summary.fail}`;

  const chips = [
    {label: "通过", value: summary.pass},
    {label: "失败", value: summary.fail},
    {label: "跳过", value: summary.skip},
    ...(summary.unconfirmed > 0 ? [{label: "未确认", value: summary.unconfirmed}] : []),
  ];

  const sectionTitle = (text: string) => (
    <div style={{fontSize: scalePx(11), fontWeight: 800, letterSpacing: "0.8px", color: "var(--color-text-dim)", marginBottom: scalePx(4)}}>
      {text}
    </div>
  );

  return (
    <div style={PAGE_STYLE}>
      <Header
        title="自动测试"
        subtitle={info.mac ? `MAC ${info.mac}` : undefined}
        showClose
        closeTo="/settings"
      />
      <div style={{padding: scalePx(14), maxWidth: scalePx(440), margin: "0 auto"}}>
        {/* 状态总览 */}
        <div style={{
          position: "relative",
          overflow: "hidden",
          borderRadius: scalePx(16),
          padding: scalePx(16),
          marginBottom: scalePx(10),
          background: runState === "aborted"
            ? "linear-gradient(135deg, #ff453a 0%, #c2410c 100%)"
            : runState === "finished" && !summary.ok
              ? "linear-gradient(135deg, #ff9f0a 0%, #f97316 100%)"
              : "linear-gradient(135deg, #0a84ff 0%, #5e5ce6 100%)",
          color: "#fff",
          boxShadow: "0 8px 24px rgba(10,132,255,0.22)",
        }}>
          <div style={{position: "absolute", width: scalePx(130), height: scalePx(130), borderRadius: "50%", background: "rgba(255,255,255,0.10)", top: scalePx(-56), right: scalePx(-34)}}/>
          <div style={{position: "absolute", width: scalePx(74), height: scalePx(74), borderRadius: "50%", background: "rgba(255,255,255,0.08)", bottom: scalePx(-34), right: scalePx(46)}}/>
          <div style={{position: "relative"}}>
            <div style={{fontSize: scalePx(9), fontWeight: 800, letterSpacing: "1.6px", opacity: 0.75}}>SELF TEST</div>
            <div style={{fontSize: scalePx(20), fontWeight: 800, marginTop: scalePx(3), lineHeight: 1.25}}>{heroTitle}</div>
            <div style={{fontSize: scalePx(11), opacity: 0.88, marginTop: scalePx(4), lineHeight: 1.55}}>{heroSubtitle}</div>

            {items.length > 0 && (
              <>
                <div style={{marginTop: scalePx(12), height: scalePx(6), borderRadius: scalePx(3), background: "rgba(255,255,255,0.25)", overflow: "hidden"}}>
                  <div style={{
                    width: `${percent}%`,
                    height: "100%",
                    borderRadius: scalePx(3),
                    background: "#fff",
                    transition: "width 0.4s ease",
                  }}/>
                </div>
                <div style={{display: "flex", flexWrap: "wrap", gap: scalePx(6), marginTop: scalePx(10)}}>
                  {chips.map(chip => (
                    <span key={chip.label} style={{
                      padding: `${scalePx(2)} ${scalePx(8)}`,
                      borderRadius: "var(--radius-full)",
                      background: "rgba(255,255,255,0.18)",
                      fontSize: scalePx(10),
                      fontWeight: 700,
                    }}>
                      {chip.label} {chip.value}
                    </span>
                  ))}
                </div>
              </>
            )}
          </div>
        </div>

        {/* 选项 */}
        <Card marginBottom={10}>
          {sectionTitle("测试选项")}
          <OptionRow
            icon="⚠️"
            title="安全确认（必选）"
            desc="车前 1 米内无障碍、机械臂周围无障碍"
            on={checked}
            disabled={running}
            onChange={setChecked}
            accent={checked ? "rgba(52,199,89,0.16)" : "rgba(245,158,11,0.16)"}
          />
          <div style={{height: 1, background: "var(--color-border-light)", margin: `${scalePx(3)} 0`}}/>
          <OptionRow
            icon="🎾"
            title="自动抓网球测试"
            desc="识别并抓取网球（需要放球）；关闭后不测该项"
            on={enableDemo}
            disabled={running}
            onChange={setEnableDemo}
          />
          <OptionRow
            icon="📶"
            title="WiFi 测试"
            desc="扫描并连接 Tsinghua-Dongsheng（不恢复原网络）"
            on={enableWifi}
            disabled={running}
            onChange={setEnableWifi}
          />
          <OptionRow
            icon="🦾"
            title="舵机测试"
            desc="底座 / 腕部 / 夹爪逐个大幅转动，立即恢复"
            on={enableArm}
            disabled={running}
            onChange={setEnableArm}
          />
          <OptionRow
            icon="🔁"
            title="动作失败后继续"
            desc="默认失败即跳过后续动作项"
            on={continueOnFailure}
            disabled={running}
            onChange={setContinueOnFailure}
          />
          <div style={{
            marginTop: scalePx(2),
            fontSize: scalePx(9),
            lineHeight: 1.5,
            color: "var(--color-text-dim)",
          }}>
            以上开关对「开始测试」和「全自动测试」都生效；动作与摄像头为必测项，全自动失败不中断。
          </div>
          {enableWifi && (
            <div style={{
              marginTop: scalePx(4),
              padding: `${scalePx(7)} ${scalePx(9)}`,
              borderRadius: scalePx(8),
              background: "rgba(245,158,11,0.10)",
              fontSize: scalePx(10),
              lineHeight: 1.55,
              color: "#b45309",
            }}>
              WiFi 测试需通过本车热点（192.168.4.1）访问；若当前走的是已连接的路由器网络，换网后页面会断开。
            </div>
          )}

          <div style={{height: 1, background: "var(--color-border-light)", margin: `${scalePx(7)} 0 ${scalePx(6)}`}}/>
          <div style={{display: "flex", alignItems: "center", gap: scalePx(10)}}>
            <span style={{
              display: "grid", placeItems: "center",
              width: scalePx(30), height: scalePx(30), borderRadius: scalePx(9),
              background: "rgba(52,199,89,0.12)", fontSize: scalePx(15), flexShrink: 0,
            }}>🎾</span>
            <div style={{flex: 1, minWidth: 0}}>
              <div style={{fontSize: scalePx(13), fontWeight: 600}}>自动抓网球参数</div>
              <div style={{fontSize: scalePx(10), color: "var(--color-text-muted)", marginTop: 1}}>默认与「自动抓网球」页一致</div>
            </div>
          </div>
          <div style={{display: "flex", gap: scalePx(8), marginTop: scalePx(7)}}>
            {DEMO_FIELDS.map(field => (
              <label key={field.key} style={{flex: 1, minWidth: 0, display: "flex", flexDirection: "column", gap: scalePx(3)}}>
                <span style={{fontSize: scalePx(10), color: "var(--color-text-muted)"}}>{field.label}</span>
                <div style={{
                  display: "flex", alignItems: "center",
                  border: "1px solid var(--color-border-light)",
                  borderRadius: scalePx(8),
                  background: "rgba(0,0,0,0.02)",
                  padding: `0 ${scalePx(7)}`,
                }}>
                  <input
                    type="number"
                    min={field.min}
                    max={field.max}
                    step={field.step}
                    value={demoForm[field.key]}
                    onChange={event => setDemoForm(prev => ({...prev, [field.key]: event.target.value}))}
                    disabled={running}
                    style={{
                      flex: 1, minWidth: 0, border: "none", outline: "none", background: "transparent",
                      padding: `${scalePx(6)} 0`,
                      fontSize: scalePx(13), fontWeight: 700, color: "inherit",
                    }}
                  />
                  <span style={{fontSize: scalePx(10), color: "var(--color-text-dim)"}}>{field.unit}</span>
                </div>
              </label>
            ))}
          </div>

          <div style={{height: 1, background: "var(--color-border-light)", margin: `${scalePx(7)} 0 ${scalePx(6)}`}}/>
          <div style={{display: "flex", alignItems: "center", gap: scalePx(10)}}>
            <span style={{
              display: "grid", placeItems: "center",
              width: scalePx(30), height: scalePx(30), borderRadius: scalePx(9),
              background: "rgba(10,132,255,0.10)", fontSize: scalePx(15), flexShrink: 0,
            }}>⚡</span>
            <div style={{flex: 1}}>
              <div style={{display: "flex", justifyContent: "space-between", fontSize: scalePx(13), fontWeight: 600}}>
                <span>动作测试速度</span>
                <span style={{color: "#0a84ff", fontWeight: 800}}>{speed}%</span>
              </div>
              <input
                type="range"
                min={10}
                max={40}
                step={5}
                value={speed}
                onChange={event => setSpeed(Number(event.target.value))}
                disabled={running}
                style={{width: "100%", accentColor: "#0a84ff", marginTop: scalePx(2)}}
              />
            </div>
          </div>

          <div style={{
            display: "flex", justifyContent: "space-between",
            marginTop: scalePx(8), paddingTop: scalePx(7),
            borderTop: "1px solid var(--color-border-light)",
            fontSize: scalePx(9), color: "var(--color-text-dim)",
          }}>
            <span>{info.ip ? `IP ${info.ip}` : "IP —"}</span>
            <span>{info.version ? `固件 ${info.version}` : ""}</span>
          </div>
        </Card>

        {precheckError && (
          <Card marginBottom={10} style={{background: "rgba(255,59,48,0.08)", border: "1px solid rgba(255,59,48,0.22)"}}>
            <div style={{fontSize: scalePx(13), color: "var(--color-danger)", fontWeight: 700}}>{precheckError}</div>
          </Card>
        )}

        {!running && (
          <Card marginBottom={10} style={{background: "rgba(245,158,11,0.08)", border: "1px solid rgba(245,158,11,0.25)"}}>
            <div style={{fontSize: scalePx(13), fontWeight: 800, color: "#b45309", marginBottom: scalePx(3)}}>
              全自动测试（无人值守）
            </div>
            <div style={{fontSize: scalePx(10), lineHeight: 1.7, color: "#b45309"}}>
              <div>按所选项目一次跑完（动作 → 摄像头 → 可选的抓球 → 可选的舵机 → 可选的 WiFi），中途无确认，失败不中断，结束自动保存历史。</div>
              <div>开始前：网球放在车前 30–60cm；车前后留 ≥1m；机械臂周围清空。</div>
              <div>只按接口成功判定，不检查转向方向 / 距离 / 夹持效果，结果标「无人值守」。</div>
              <div>勾选 WiFi 时会切换并保留到 Tsinghua-Dongsheng；请通过本车热点 192.168.4.1 打开本页。</div>
            </div>
          </Card>
        )}

        {/* 主按钮 */}
        <div style={{display: "flex", gap: scalePx(8), marginBottom: scalePx(10)}}>
          {running ? (
            <>
              <button
                disabled
                style={{
                  flex: 1,
                  padding: `${scalePx(13)} 0`,
                  border: "none",
                  borderRadius: scalePx(13),
                  background: "linear-gradient(135deg, #0a84ff, #5e5ce6)",
                  color: "#fff",
                  fontSize: scalePx(15),
                  fontWeight: 800,
                  cursor: "not-allowed",
                  opacity: 0.75,
                }}
              >
                {runState === "precheck"
                  ? "预检中…"
                  : runMode === "unattended" ? "全自动进行中…" : "测试进行中…"}
              </button>
              <button
                onClick={abort}
                style={{
                  padding: `0 ${scalePx(18)}`,
                  border: "none",
                  borderRadius: scalePx(13),
                  background: "var(--color-danger)",
                  color: "#fff",
                  fontSize: scalePx(14),
                  fontWeight: 700,
                  cursor: "pointer",
                }}
              >
                中止
              </button>
            </>
          ) : (
            <>
              <button
                onClick={start}
                disabled={!checked}
                style={{
                  flex: 1,
                  padding: `${scalePx(13)} 0`,
                  border: "none",
                  borderRadius: scalePx(13),
                  background: checked
                    ? "linear-gradient(135deg, #0a84ff, #5e5ce6)"
                    : "#c7c7cc",
                  color: "#fff",
                  fontSize: scalePx(15),
                  fontWeight: 800,
                  cursor: checked ? "pointer" : "not-allowed",
                  boxShadow: checked ? "0 4px 16px rgba(10,132,255,0.35)" : "none",
                  transition: "background 0.2s, box-shadow 0.2s",
                }}
              >
                {runState === "idle" ? "开始测试" : "重新测试"}
              </button>
              <button
                onClick={startUnattended}
                disabled={!checked}
                style={{
                  flex: 1,
                  padding: `${scalePx(13)} 0`,
                  border: "none",
                  borderRadius: scalePx(13),
                  background: checked
                    ? "linear-gradient(135deg, #30d158, #0a84ff)"
                    : "#c7c7cc",
                  color: "#fff",
                  fontSize: scalePx(15),
                  fontWeight: 800,
                  cursor: checked ? "pointer" : "not-allowed",
                  boxShadow: checked ? "0 4px 16px rgba(48,209,88,0.30)" : "none",
                  transition: "background 0.2s, box-shadow 0.2s",
                }}
              >
                全自动测试
              </button>
            </>
          )}
        </div>

        {armSnapshot && !running && (
          <button
            onClick={restoreArm}
            style={{
              width: "100%",
              marginBottom: scalePx(10),
              padding: `${scalePx(9)} 0`,
              borderRadius: scalePx(11),
              border: "1px solid rgba(10,132,255,0.30)",
              background: "rgba(10,132,255,0.06)",
              color: "#0a84ff",
              fontSize: scalePx(12),
              fontWeight: 700,
              cursor: "pointer",
            }}
          >
            🦾 一键恢复机械臂原始角度
          </button>
        )}

        {/* 测试明细 / 流程 */}
        <Card marginBottom={10}>
          {items.length === 0 ? (
            <>
              {sectionTitle("测试流程")}
              {FLOW_ITEMS.map(flow => (
                <div key={flow.title} style={{display: "flex", gap: scalePx(10), padding: `${scalePx(6)} 0`}}>
                  <span style={{fontSize: scalePx(16), lineHeight: 1.3}}>{flow.icon}</span>
                  <div style={{flex: 1}}>
                    <div style={{fontSize: scalePx(12), fontWeight: 700}}>{flow.title}</div>
                    <div style={{fontSize: scalePx(10), lineHeight: 1.5, color: "var(--color-text-muted)", marginTop: 1}}>{flow.desc}</div>
                  </div>
                </div>
              ))}
            </>
          ) : (
            <>
              <div style={{display: "flex", alignItems: "center", justifyContent: "space-between", marginBottom: scalePx(2)}}>
                {sectionTitle("测试明细")}
                <span style={{fontSize: scalePx(10), color: "var(--color-text-dim)", marginBottom: scalePx(4)}}>
                  {done}/{items.length} 项
                </span>
              </div>
              {(runState === "finished" || runState === "aborted") && (
                <div style={{fontSize: scalePx(10), color: "var(--color-text-dim)", marginBottom: scalePx(5)}}>
                  点每一项右侧的 ↻ 可单独重测该项
                </div>
              )}
              <ItemList
                items={items}
                cameraSrc={cameraSrc}
                onCameraLoad={() => frameWaiterRef.current?.(true)}
                onCameraError={() => frameWaiterRef.current?.(false)}
                onRetest={checked && !running ? retest : undefined}
                retestingId={retestId}
              />
            </>
          )}
        </Card>

        {(runState === "finished" || runState === "aborted" || history.length > 0) && (
          <Card marginBottom={10}>
            <HistoryPanel
              records={history}
              remark={remark}
              saved={saved}
              autoSaved={autoSaved}
              canSave={canSave}
              onChangeRemark={setRemark}
              onSave={saveRecord}
              onExport={exportRecord}
              onClear={() => setShowClear(true)}
            />
          </Card>
        )}

        <div style={{height: scalePx(24)}}/>
      </div>

      <ConfirmDialog
        open={confirmMessage !== null}
        title="请确认"
        message={confirmMessage ?? ""}
        confirmText="通过"
        cancelText="失败"
        onConfirm={() => answerConfirm(true)}
        onCancel={() => answerConfirm(false)}
      />

      <NoticeDialog
        open={noticeMessage !== null}
        title="准备自动抓网球"
        message={noticeMessage ?? ""}
        hint="网球请放在车前 30–60cm 的硬质地面上，避开强光和反光；放好后人员退到安全距离。"
        confirmText="已放好，开始抓取"
        cancelText="跳过本项"
        onConfirm={() => answerNotice(true)}
        onCancel={() => answerNotice(false)}
      />

      <ConfirmDialog
        open={showClear}
        title="清空测试历史"
        message="确定要清空本机保存的全部测试记录吗？"
        confirmText="清空"
        cancelText="取消"
        danger
        onConfirm={clearAll}
        onCancel={() => setShowClear(false)}
      />
    </div>
  );
};

export default SelfTestPage;
