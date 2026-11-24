import {useViewportScale} from "../../hooks/useViewportScale";
import type { SelfTestRecord } from "../core.ts";

interface HistoryPanelProps {
  records: SelfTestRecord[];
  remark: string;
  saved: boolean;
  /** 本次记录是全自动结束后自动写入的 */
  autoSaved?: boolean;
  canSave: boolean;
  onChangeRemark: (value: string) => void;
  onSave: () => void;
  onExport: (record: SelfTestRecord) => void;
  onClear: () => void;
}

const formatTime = (ts: number) => new Date(ts).toLocaleString("zh-CN", {hour12: false});

const HistoryPanel = ({
  records, remark, saved, autoSaved = false, canSave, onChangeRemark, onSave, onExport, onClear,
}: HistoryPanelProps) => {
  const {scalePx} = useViewportScale();

  return (
    <div>
      <div style={{fontSize: scalePx(14), fontWeight: 700, marginTop: scalePx(14), marginBottom: scalePx(6)}}>
        本次测试备注
      </div>
      <textarea
        value={remark}
        onChange={event => onChangeRemark(event.target.value)}
        placeholder="例如：左转 90° 略偏、夹爪正常"
        rows={3}
        style={{
          width: "100%",
          boxSizing: "border-box",
          padding: scalePx(8),
          borderRadius: "var(--radius-sm)",
          border: "1px solid var(--color-border)",
          background: "var(--color-bg-card)",
          color: "var(--color-text)",
          fontSize: scalePx(13),
          resize: "vertical",
        }}
      />
      <button
        onClick={onSave}
        disabled={!canSave}
        style={{
          width: "100%",
          marginTop: scalePx(8),
          padding: `${scalePx(10)} 0`,
          border: "none",
          borderRadius: "var(--radius-md)",
          background: saved ? "var(--color-success)" : "var(--color-primary)",
          color: "#fff",
          fontSize: scalePx(14),
          fontWeight: 600,
          cursor: canSave ? "pointer" : "not-allowed",
          opacity: canSave ? 1 : 0.5,
        }}
      >
        {saved ? (autoSaved ? "✓ 已自动保存（无人值守）" : "✓ 已保存") : "保存本次记录"}
      </button>

      <div style={{display: "flex", alignItems: "center", marginTop: scalePx(16), marginBottom: scalePx(4)}}>
        <span style={{flex: 1, fontSize: scalePx(14), fontWeight: 700}}>测试历史（{records.length}）</span>
        {records.length > 0 && (
          <button
            onClick={onClear}
            style={{
              border: "none",
              background: "none",
              color: "var(--color-danger)",
              fontSize: scalePx(12),
              cursor: "pointer",
              padding: scalePx(4),
            }}
          >
            清空
          </button>
        )}
      </div>

      {records.length === 0 && (
        <div style={{fontSize: scalePx(12), color: "var(--color-text-dim)"}}>暂无记录</div>
      )}

      {records.map(record => (
        <div key={record.id} style={{padding: `${scalePx(6)} 0`, borderBottom: "1px solid var(--color-border-light)"}}>
          <div style={{display: "flex", alignItems: "center", gap: scalePx(6)}}>
            <span style={{flex: 1, fontSize: scalePx(12), color: "var(--color-text-muted)"}}>{formatTime(record.ts)}</span>
            {record.mode === "unattended" && (
              <span style={{
                fontSize: scalePx(9),
                fontWeight: 700,
                color: "#0a84ff",
                background: "rgba(10,132,255,0.10)",
                borderRadius: "var(--radius-full)",
                padding: `${scalePx(1)} ${scalePx(6)}`,
              }}>全自动</span>
            )}
            <span style={{fontSize: scalePx(11), color: "var(--color-text-dim)"}}>
              通过 {record.summary.pass} / 失败 {record.summary.fail}
              {record.summary.skip > 0 ? ` / 跳过 ${record.summary.skip}` : ""}
              {record.summary.unconfirmed > 0 ? ` / 未确认 ${record.summary.unconfirmed}` : ""}
            </span>
          </div>
          {record.remark && (
            <div style={{fontSize: scalePx(12), color: "var(--color-text)", marginTop: 2}}>{record.remark}</div>
          )}
          <button
            onClick={() => onExport(record)}
            style={{
              marginTop: scalePx(3),
              border: "1px solid var(--color-border)",
              background: "var(--color-bg-card)",
              color: "var(--color-text-dim)",
              borderRadius: "var(--radius-sm)",
              fontSize: scalePx(11),
              padding: `${scalePx(2)} ${scalePx(8)}`,
              cursor: "pointer",
            }}
          >
            导出 JSON
          </button>
        </div>
      ))}
    </div>
  );
};

export default HistoryPanel;
