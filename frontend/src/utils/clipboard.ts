/**
 * 复制文本到剪贴板。
 * 优先用 Clipboard API；自签证书/非安全上下文或被拒时，退回 execCommand。
 */
export async function copyToClipboard(text: string): Promise<boolean> {
    try {
        if (navigator.clipboard?.writeText) {
            await navigator.clipboard.writeText(text);
            return true;
        }
    } catch {
        // 继续走兜底方案
    }
    try {
        const ta = document.createElement("textarea");
        ta.value = text;
        ta.setAttribute("readonly", "");
        ta.style.position = "fixed";
        ta.style.top = "0";
        ta.style.left = "0";
        ta.style.opacity = "0";
        document.body.appendChild(ta);
        ta.select();
        ta.setSelectionRange(0, text.length);
        const ok = document.execCommand("copy");
        document.body.removeChild(ta);
        return ok;
    } catch {
        return false;
    }
}

/** 是否是可用于拼接 URL 的 IPv4 地址（排除 0.0.0.0 / 127.0.0.1） */
export function isValidIPv4(ip: string): boolean {
    return /^\d{1,3}(\.\d{1,3}){3}$/.test(ip) && ip !== "0.0.0.0" && ip !== "127.0.0.1";
}

/** 车机 Web 访问地址（沿用当前页面的协议，热点/局域网都适用） */
export function carUrl(ip: string): string {
    return `${window.location.protocol}//${ip}/`;
}
