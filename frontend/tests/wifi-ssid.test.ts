import test from "node:test";
import assert from "node:assert/strict";
import {cleanWifiSsid, decodeWifiSsid} from "../src/utils/wifiSsid.ts";

// 测试数据取自车 A 实测 /api/wifi/scan（旧 capp 不做 \xHH 解码）
test("decodeWifiSsid: 还原 \\xHH 中文 SSID", () => {
    assert.equal(
        decodeWifiSsid("\\xe7\\xb1\\xb3\\xe5\\xa1\\x94\\xe8\\xa7\\x86\\xe7\\x95\\x8c"),
        "米塔视界",
    );
    assert.equal(decodeWifiSsid("oneone"), "oneone");
});

test("decodeWifiSsid: 中英混排可还原，已解码的名称保持不变", () => {
    assert.equal(
        decodeWifiSsid("AI\\xe5\\x8e\\x9f\\xe7\\x82\\xb9\\xe7\\xa4\\xbe\\xe5\\x8c\\xba\\xe5\\x88\\x9b\\xe6\\x96\\xb0\\xe7\\xa9\\xba\\xe9\\x97\\xb4_IPv6"),
        "AI原点社区创新空间_IPv6",
    );
    assert.equal(decodeWifiSsid("家Home"), "家Home");
});

test("decodeWifiSsid: 非法转义序列原样保留", () => {
    assert.equal(decodeWifiSsid("\\xZZ\\x41"), "\\xZZA");
});

test("cleanWifiSsid: 隐藏网络的 NUL 填充会被清掉", () => {
    assert.equal(cleanWifiSsid("\\x00\\x00\\x00\\x00"), "");
    assert.equal(cleanWifiSsid("\\xe7\\xb1\\xb3\\xe5\\xa1\\x94\\xe8\\xa7\\x86\\xe7\\x95\\x8c"), "米塔视界");
});
