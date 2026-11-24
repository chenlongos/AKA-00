#!/usr/bin/env python3
"""把 wpa_cli 输出里的 \\xHH 还原成 UTF-8 字节（等价于 capp 上游的 decode_ssid）。

wpa_supplicant 的 wpa_cli 用 printf_encode 输出非 ASCII 的 SSID：
「中文」会变成 "\\xe4\\xb8\\xad\\xe6\\x96\\x87"。现网固件的 aka-capp 二进制
没有上游 00cc52e 的解码逻辑，于是 /api/wifi/scan|status 把它原样交给前端，
中文 WiFi 名显示成一串 \\x，连接也就失败。这里在进程外补上这一步。

只还原可打印字节（>=0x20 且 !=0x7F）：控制字符（\\x09 制表、\\x0a 换行）
保持转义，否则 SSID 里的它们会在 capp 按行/按 tab 解析之前变成真分隔符，
把扫描列表结构搅乱。非法转义（\\xZZ 之类）原样保留。

用法: wpa_cli_decode.py <真实 wpa_cli 路径> [wpa_cli 参数...]
退出码与真实 wpa_cli 一致；stdout 用二进制写，不经过文本编码。
"""
import re
import subprocess
import sys


def _decode(match):
    value = int(match.group(1), 16)
    if value >= 0x20 and value != 0x7F:
        return bytes((value,))
    return match.group(0)


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("usage: wpa_cli_decode.py <real-wpa-cli> [args...]\n")
        return 2
    real, args = argv[1], argv[2:]
    try:
        proc = subprocess.Popen([real] + args, stdout=subprocess.PIPE)
    except OSError as exc:
        sys.stderr.write("wpa_cli: %s\n" % exc)
        return 127
    data = proc.communicate()[0]
    sys.stdout.buffer.write(re.sub(rb"\\x([0-9A-Fa-f]{2})", _decode, data))
    sys.stdout.buffer.flush()
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv))
