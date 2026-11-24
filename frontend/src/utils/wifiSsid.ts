/**
 * 旧固件的 SSID 兜底解码。
 *
 * wpa_cli 对非 ASCII 的 SSID 输出 \xHH 转义序列（如 "\xe7\xb1\xb3" 表示 UTF-8 的
 * "米"）。capp 源码里已有 decode_ssid（2026-11-03 提交 00cc52e），但车上的旧二进制
 * 没有，/api/wifi/scan 与 /api/wifi/status 会把转义文本原样返回 —— 页面显示乱码，
 * 点连接时又把转义文本当 SSID 下发，wpa_supplicant 找不到网络。
 *
 * 这里做幂等兜底：只有出现 \xHH 才解码；新固件返回的真实中文原样通过。
 */
export function decodeWifiSsid(raw: string): string {
    if (!raw.includes("\\x")) return raw;
    const bytes: number[] = [];
    const encoder = new TextEncoder();
    let i = 0;
    while (i < raw.length) {
        if (raw[i] === "\\" && raw[i + 1] === "x" && i + 3 < raw.length
            && /^[0-9a-fA-F]{2}$/.test(raw.slice(i + 2, i + 4))) {
            bytes.push(parseInt(raw.slice(i + 2, i + 4), 16));
            i += 4;
            continue;
        }
        const cp = raw.codePointAt(i);
        if (cp === undefined) break;
        const ch = String.fromCodePoint(cp);
        bytes.push(...encoder.encode(ch));
        i += ch.length;
    }
    return new TextDecoder("utf-8", {fatal: false}).decode(new Uint8Array(bytes));
}

/** 解码并去掉 NUL 等控制字符（隐藏网络的 SSID 是 \x00 填充，清完为空串）。 */
export function cleanWifiSsid(raw: string): string {
    return decodeWifiSsid(raw).replace(/[\u0000-\u001f\u007f]/g, "");
}
