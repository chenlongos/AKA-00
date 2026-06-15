import React from "react";

interface ConfirmDialogProps {
    open: boolean;
    title?: string;
    message: string;
    confirmText?: string;
    cancelText?: string;
    onConfirm: () => void;
    onCancel: () => void;
    /** 是否为警告/危险操作（红色确认按钮） */
    danger?: boolean;
}

const ConfirmDialog: React.FC<ConfirmDialogProps> = ({
    open, title, message, confirmText = "确定", cancelText = "取消",
    onConfirm, onCancel, danger = false,
}) => {
    if (!open) return null;

    return (
        <div
            style={{
                position: "fixed", inset: 0, zIndex: 9999,
                display: "flex", alignItems: "center", justifyContent: "center",
                padding: "30px",
            }}
            onClick={onCancel}
        >
            {/* 遮罩 */}
            <div style={{
                position: "absolute", inset: 0,
                background: "rgba(0,0,0,0.6)",
                animation: "fadeIn 0.15s ease-out",
            }} />

            {/* 弹窗 */}
            <div
                onClick={e => e.stopPropagation()}
                style={{
                    position: "relative",
                    background: "var(--color-bg-card, #1e293b)",
                    borderRadius: 14,
                    padding: "24px 20px 16px",
                    maxWidth: 300,
                    width: "100%",
                    boxShadow: "0 20px 60px rgba(0,0,0,0.5)",
                    animation: "fadeIn 0.2s ease-out",
                    color: "var(--color-text, #f1f5f9)",
                }}
            >
                {title && (
                    <div style={{fontSize: 17, fontWeight: 700, marginBottom: 8, textAlign: "center"}}>
                        {title}
                    </div>
                )}
                <div style={{fontSize: 15, lineHeight: 1.6, textAlign: "center", color: "var(--color-text-muted, #94a3b8)", marginBottom: 20}}>
                    {message}
                </div>
                <div style={{display: "flex", gap: 10}}>
                    <button
                        onClick={onCancel}
                        style={{
                            flex: 1, padding: "12px 0", borderRadius: 10,
                            border: "none", background: "var(--color-bg-elevated, #334155)",
                            color: "var(--color-text, #f1f5f9)", fontSize: 16, fontWeight: 600,
                            cursor: "pointer", outline: "none",
                        }}
                    >
                        {cancelText}
                    </button>
                    <button
                        onClick={onConfirm}
                        style={{
                            flex: 1, padding: "12px 0", borderRadius: 10,
                            border: "none",
                            background: danger ? "#ef4444" : "#2563eb",
                            color: "#fff", fontSize: 16, fontWeight: 700,
                            cursor: "pointer", outline: "none",
                        }}
                    >
                        {confirmText}
                    </button>
                </div>
            </div>
        </div>
    );
};

export default ConfirmDialog;
