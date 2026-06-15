import React from "react";

interface AlertDialogProps {
    open: boolean;
    title?: string;
    message: string;
    buttonText?: string;
    onClose: () => void;
}

const AlertDialog: React.FC<AlertDialogProps> = ({
    open, title, message, buttonText = "确定", onClose,
}) => {
    if (!open) return null;

    return (
        <div
            style={{
                position: "fixed", inset: 0, zIndex: 9999,
                display: "flex", alignItems: "center", justifyContent: "center",
                padding: "30px",
            }}
            onClick={onClose}
        >
            <div style={{
                position: "absolute", inset: 0,
                background: "rgba(0,0,0,0.6)",
            }} />
            <div
                onClick={e => e.stopPropagation()}
                style={{
                    position: "relative",
                    background: "var(--color-bg-card, #1e293b)",
                    borderRadius: 14, padding: "24px 20px 16px",
                    maxWidth: 300, width: "100%",
                    boxShadow: "0 20px 60px rgba(0,0,0,0.5)",
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
                <button
                    onClick={onClose}
                    style={{
                        width: "100%", padding: "12px 0", borderRadius: 10,
                        border: "none", background: "#2563eb", color: "#fff",
                        fontSize: 16, fontWeight: 700,
                        cursor: "pointer", outline: "none",
                    }}
                >
                    {buttonText}
                </button>
            </div>
        </div>
    );
};

export default AlertDialog;
