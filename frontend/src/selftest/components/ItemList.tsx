import {useEffect, useRef} from "react";
import {useViewportScale} from "../../hooks/useViewportScale";
import type { ItemResult, ItemStatus } from "../core.ts";

const GROUP_LABELS: Record<string, string> = {
  motion: "动作",
  camera: "摄像头",
  demo: "自动抓网球",
  wifi: "WiFi",
  arm: "舵机角度",
};

const GROUP_HINTS: Record<string, string> = {
  motion: "底盘移动 + 夹取",
  camera: "画面与关闭",
  demo: "识别并抓取",
  wifi: "扫描 / 连接 / 验证",
  arm: "三个舵机逐个转动",
};

const STATUS: Record<ItemStatus, {text: string; color: string; bg: string}> = {
  pending: {text: "待测", color: "#8e8e93", bg: "rgba(142,142,147,0.12)"},
  running: {text: "运行中", color: "#0a84ff", bg: "rgba(10,132,255,0.12)"},
  confirm: {text: "待确认", color: "#b45309", bg: "rgba(245,158,11,0.16)"},
  pass: {text: "通过", color: "#15803d", bg: "rgba(52,199,89,0.14)"},
  fail: {text: "失败", color: "#dc2626", bg: "rgba(255,59,48,0.12)"},
  skip: {text: "跳过", color: "#8e8e93", bg: "rgba(142,142,147,0.10)"},
  unconfirmed: {text: "未确认", color: "#b45309", bg: "rgba(245,158,11,0.14)"},
};

interface ItemListProps {
  items: ItemResult[];
  /** 摄像头打开时挂上画面，画面会显示在「摄像头 打开 / 显示」这一行下方 */
  cameraSrc?: string;
  onCameraLoad?: () => void;
  onCameraError?: () => void;
  /** 整套流程结束后传入：点某一行右侧的 ↻ 单独重测该项 */
  onRetest?: (id: string) => void;
  /** 正在重测的项（那一行不再显示 ↻） */
  retestingId?: string | null;
}

const ItemList = ({items, cameraSrc, onCameraLoad, onCameraError, onRetest, retestingId}: ItemListProps) => {
  const {scalePx} = useViewportScale();
  const itemRefs = useRef<Record<string, HTMLDivElement | null>>({});
  const activeId = items.find(item => item.status === "running" || item.status === "confirm")?.id ?? null;

  // 自动滚动：当前执行/待确认的项滚到屏幕中间，操作员不用自己找
  useEffect(() => {
    if (!activeId) return;
    itemRefs.current[activeId]?.scrollIntoView({behavior: "smooth", block: "center"});
  }, [activeId]);

  const groups = items.reduce<string[]>((acc, item) => {
    if (!acc.includes(item.group)) acc.push(item.group);
    return acc;
  }, []);

  return (
    <div>
      {groups.map((group, groupIndex) => (
        <div key={group} style={{marginTop: groupIndex === 0 ? scalePx(2) : scalePx(14)}}>
          <div style={{display: "flex", alignItems: "center", gap: scalePx(7), marginBottom: scalePx(6)}}>
            <span style={{
              display: "grid",
              placeItems: "center",
              width: scalePx(18),
              height: scalePx(18),
              borderRadius: scalePx(6),
              background: "rgba(10,132,255,0.12)",
              color: "#0a84ff",
              fontSize: scalePx(10),
              fontWeight: 800,
            }}>{groupIndex + 1}</span>
            <span style={{fontSize: scalePx(13), fontWeight: 700}}>{GROUP_LABELS[group] ?? group}</span>
            <span style={{fontSize: scalePx(10), color: "var(--color-text-dim)"}}>{GROUP_HINTS[group]}</span>
          </div>

          {items.filter(item => item.group === group).map(item => {
            const tone = STATUS[item.status];
            const isActive = activeId === item.id;
            return (
              <div
                key={item.id}
                ref={el => { itemRefs.current[item.id] = el; }}
                style={{
                  position: "relative",
                  padding: `${scalePx(9)} ${scalePx(10)}`,
                  marginBottom: scalePx(6),
                  borderRadius: scalePx(10),
                  background: isActive ? "rgba(10,132,255,0.06)" : "rgba(0,0,0,0.015)",
                  border: isActive ? "1px solid rgba(10,132,255,0.35)" : "1px solid transparent",
                  transition: "background 0.2s, border-color 0.2s",
                }}
              >
                <div style={{display: "flex", alignItems: "center", gap: scalePx(7)}}>
                  <span style={{
                    width: scalePx(7),
                    height: scalePx(7),
                    borderRadius: "50%",
                    background: tone.color,
                    flexShrink: 0,
                    animation: item.status === "running" ? "pulse 1.1s ease-in-out infinite" : undefined,
                  }}/>
                  <span style={{flex: 1, fontSize: scalePx(13), fontWeight: 600}}>{item.name}</span>
                  {item.elapsedMs > 0 && (
                    <span style={{fontSize: scalePx(10), color: "var(--color-text-dim)", fontVariantNumeric: "tabular-nums"}}>
                      {(item.elapsedMs / 1000).toFixed(1)}s
                    </span>
                  )}
                  <span style={{
                    fontSize: scalePx(10),
                    fontWeight: 700,
                    color: tone.color,
                    background: tone.bg,
                    borderRadius: "var(--radius-full)",
                    padding: `${scalePx(2)} ${scalePx(7)}`,
                    minWidth: scalePx(44),
                    textAlign: "center",
                  }}>
                    {tone.text}
                  </span>
                  {onRetest && item.id !== retestingId && item.status !== "running" && item.status !== "confirm" && (
                    <button
                      onClick={() => onRetest(item.id)}
                      title="重测本项"
                      style={{
                        flexShrink: 0,
                        display: "grid",
                        placeItems: "center",
                        width: scalePx(22),
                        height: scalePx(20),
                        borderRadius: scalePx(6),
                        border: "1px solid rgba(10,132,255,0.35)",
                        background: "rgba(10,132,255,0.06)",
                        color: "#0a84ff",
                        fontSize: scalePx(12),
                        fontWeight: 700,
                        cursor: "pointer",
                        lineHeight: 1,
                        padding: 0,
                      }}
                    >
                      ↻
                    </button>
                  )}
                </div>

                {item.steps.length > 0 && (
                  <div style={{
                    marginTop: scalePx(5),
                    paddingLeft: scalePx(14),
                    display: "flex",
                    flexDirection: "column",
                    gap: scalePx(2),
                  }}>
                    {item.steps.map((step, index) => (
                      <div
                        key={index}
                        style={{
                          display: "flex",
                          gap: scalePx(5),
                          fontSize: scalePx(10),
                          lineHeight: 1.5,
                          color: step.ok ? "var(--color-text-muted)" : "var(--color-danger)",
                        }}
                      >
                        <span style={{flexShrink: 0}}>{step.ok ? "✓" : "✗"}</span>
                        <span>
                          {step.name}
                          {step.detail ? ` · ${step.detail}` : ""}
                          {step.error ? ` — ${step.error}` : ""}
                        </span>
                      </div>
                    ))}
                  </div>
                )}

                {item.error && (
                  <div style={{fontSize: scalePx(10), color: "var(--color-danger)", marginTop: scalePx(3), paddingLeft: scalePx(14)}}>
                    {item.error}
                  </div>
                )}

                {item.id === "camera_on" && cameraSrc && (
                  <div style={{
                    marginTop: scalePx(8),
                    borderRadius: scalePx(8),
                    overflow: "hidden",
                    position: "relative",
                    background: "#000",
                    border: "1px solid rgba(0,0,0,0.10)",
                  }}>
                    <img
                      src={cameraSrc}
                      alt="摄像头画面"
                      onLoad={onCameraLoad}
                      onError={onCameraError}
                      style={{width: "100%", maxHeight: scalePx(210), objectFit: "cover", display: "block"}}
                    />
                    <span style={{
                      position: "absolute",
                      top: scalePx(6),
                      left: scalePx(6),
                      padding: `${scalePx(2)} ${scalePx(7)}`,
                      borderRadius: "var(--radius-full)",
                      background: "rgba(0,0,0,0.55)",
                      color: "#4ade80",
                      fontSize: scalePx(9),
                      fontWeight: 700,
                    }}>● 实时画面</span>
                  </div>
                )}
              </div>
            );
          })}
        </div>
      ))}
    </div>
  );
};

export default ItemList;
