//! WiFi 路由 —— 扫描 / 连接 / 状态
//!
//! 对应 `app/routes/wifi.py`。前端 `WiFiConfigPage.tsx` 调用：
//!   GET  /status         → {ssid, ip}
//!   GET  /ip             → {ip}
//!   GET  /scan           → {list:[{id, ssid, signal, secured, is_connected}], connected}
//!   POST /connect        → text "<status>|<message>"  e.g. "success|连接成功! IP: 192.168.1.42"
//!
//! 实现：底层通过 wpa_supplicant / wpa_cli / udhcpc 控制 wlan1 STA 接口。
//! 板子启动时由 init_ap_web.sh 创建 wlan1 并起 wpa_supplicant。
//! 开发机 (macOS / Windows) 没有这些工具，自动降级返回空数据。

use std::sync::Arc;
use std::time::Duration;

use axum::{
    extract::State, http::StatusCode, response::IntoResponse, Json, Router,
};
use axum::routing::{get, post};
use base64::Engine;
use serde::Deserialize;
use tokio::process::Command;

use crate::AppState;

const WIFI_INTERFACE: &str = "wlan1";
const WIFI_CTRL_PATH: &str = "/var/run/wpa_supplicant";

/// 是否在能用 wpa_cli 的平台。dev 平台（macOS / Windows）没有 wpa_supplicant，
/// 直接返回空响应让前端知道 WiFi 不可用。
fn wifi_available() -> bool {
    cfg!(target_os = "linux")
}

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/status", get(wifi_status))
        .route("/ip", get(get_ip))
        .route("/scan", get(wifi_scan))
        .route("/connect", post(wifi_connect))
}

// ── shell 命令封装 ──

/// 同步 wait 的简化版：超时 8s（扫描等命令可能要 2-3s），失败返回空串。
/// 同时读 stdout + stderr 合并返回 —— wpa_cli 有些错误（FAIL）写到 stderr
/// 而不是 stdout，单纯读 stdout 会得到空串让上层误判。
async fn cmd_capture(args: &[&str]) -> String {
    if args.is_empty() {
        return String::new();
    }
    let mut c = Command::new(args[0]);
    for a in &args[1..] {
        c.arg(a);
    }
    c.kill_on_drop(true);
    c.stdin(std::process::Stdio::null());
    c.stdout(std::process::Stdio::piped());
    c.stderr(std::process::Stdio::piped());

    match tokio::time::timeout(Duration::from_secs(8), c.output()).await {
        Ok(Ok(out)) => {
            let stdout = String::from_utf8_lossy(&out.stdout);
            let stderr = String::from_utf8_lossy(&out.stderr);
            let exit = out.status.code();

            if !out.status.success() {
                // 失败：优先 stderr（wpa_cli 把 FAIL 等错误信息写到 stderr）
                log::warn!(
                    "[wifi] {:?} exit={:?} stderr={}",
                    args, exit, stderr.trim()
                );
                let s = if !stderr.trim().is_empty() {
                    stderr.trim().to_string()
                } else {
                    stdout.trim().to_string()
                };
                return s;
            }
            // 成功：返回 stdout，stderr 非空也带上（部分 wpa_cli 命令会混合输出）
            if stderr.trim().is_empty() {
                stdout.trim().to_string()
            } else {
                format!("{}\n[stderr] {}", stdout.trim(), stderr.trim())
            }
        }
        Ok(Err(e)) => {
            log::warn!("[wifi] {:?} spawn err: {}", args, e);
            String::new()
        }
        Err(_) => {
            log::warn!("[wifi] {:?} timeout", args);
            String::new()
        }
    }
}

/// fire-and-forget：失败也无所谓（如 killall wpa_supplicant）。
/// stdout/stderr 重定向到 /dev/null —— 否则 wpa_supplicant 启动日志
/// （"Successfully initialized wpa_supplicant" / "Could not read interface
/// wlan1 flags" 等）会通过 Command 的 inherited stdio 直接漏到 web-server
/// 日志里污染输出。
async fn cmd_run(args: &[&str]) {
    if args.is_empty() {
        return;
    }
    let mut c = Command::new(args[0]);
    for a in &args[1..] {
        c.arg(a);
    }
    c.kill_on_drop(true);
    c.stdin(std::process::Stdio::null());
    c.stdout(std::process::Stdio::null());
    c.stderr(std::process::Stdio::null());
    let _ = c.status().await;
}

// ── wpa_supplicant 生命周期 ──

async fn ensure_wpa_env() -> bool {
    if !wifi_available() {
        return false;
    }

    // 自愈：wlan1 不存在就现场创建（init_ap_web.sh 没跑过的情况下也需要工作）。
    // phy0 在 SG2002 上是板载 wifi；先看 iw dev 列出来的接口，没有 wlan1
    // 就 iw phy phy0 interface add wlan1 type managed 建一个。
    let iw_out = cmd_capture(&["iw", "dev"]).await;
    if !iw_out.lines().any(|l| l.trim().starts_with("Interface ") && l.contains(WIFI_INTERFACE)) {
        log::info!("[wifi] wlan1 not found, creating via 'iw phy phy0 interface add ...'");
        let _ = cmd_run(&[
            "iw", "phy", "phy0", "interface", "add", WIFI_INTERFACE, "type", "managed",
        ]).await;
        tokio::time::sleep(Duration::from_millis(500)).await;
    }

    // 确保 ctrl 目录存在
    let _ = tokio::fs::create_dir_all(WIFI_CTRL_PATH).await;

    let socket_file = format!("{}/{}", WIFI_CTRL_PATH, WIFI_INTERFACE);
    if tokio::fs::metadata(&socket_file).await.is_ok() {
        return true;
    }

    // 重启 wpa_supplicant
    cmd_run(&["killall", "-9", "wpa_supplicant"]).await;
    tokio::time::sleep(Duration::from_millis(500)).await;
    let _ = tokio::fs::remove_file(&socket_file).await;

    let _ = cmd_run(&["ip", "link", "set", WIFI_INTERFACE, "down"]).await;
    let _ = cmd_run(&["ip", "link", "set", WIFI_INTERFACE, "up"]).await;
    tokio::time::sleep(Duration::from_millis(500)).await;

    let _ = cmd_run(&[
        "wpa_supplicant",
        "-D",
        "nl80211",
        "-i",
        WIFI_INTERFACE,
        "-C",
        WIFI_CTRL_PATH,
        "-B",
    ])
    .await;

    // 等 socket 出现
    for _ in 0..10 {
        if tokio::fs::metadata(&socket_file).await.is_ok() {
            return true;
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
    false
}

async fn wpa_cli(args: &[&str]) -> String {
    let mut full = vec!["wpa_cli", "-p", WIFI_CTRL_PATH, "-i", WIFI_INTERFACE];
    full.extend_from_slice(args);
    cmd_capture(&full).await
}

async fn current_wifi_ip() -> String {
    let raw = cmd_capture(&["ip", "addr", "show", WIFI_INTERFACE]).await;
    for line in raw.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("inet ") {
            // "192.168.1.42/24 brd ..."
            return rest.split_whitespace().next().unwrap_or("").to_string();
        }
    }
    String::new()
}

/// wpa_cli 对非 ASCII SSID 返回 hex 转义序列，如 \\xe4\\xbb\\x95 → 仕
fn decode_ssid(raw: &str) -> String {
    if !raw.contains("\\x") {
        return raw.to_string();
    }
    let hex_str: String = raw.replace("\\x", "");
    let bytes: Vec<u8> = (0..hex_str.len())
        .step_by(2)
        .filter_map(|i| u8::from_str_radix(&hex_str[i..(i + 2).min(hex_str.len())], 16).ok())
        .collect();
    String::from_utf8(bytes).unwrap_or_else(|_| raw.to_string())
}

fn parse_connected_ssid(status: &str) -> Option<String> {
    if !status.contains("wpa_state=COMPLETED") {
        return None;
    }
    for line in status.lines() {
        if let Some(s) = line.strip_prefix("ssid=") {
            return Some(s.to_string());
        }
    }
    None
}

// ── 路由 handler ──

async fn get_ip(State(_s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    if !wifi_available() {
        return Json(serde_json::json!({ "ip": "127.0.0.1" }));
    }
    let status = wpa_cli(&["status"]).await;
    let ssid = parse_connected_ssid(&status);
    let ip = if ssid.is_some() {
        current_wifi_ip().await
    } else {
        "192.168.4.1".to_string() // AP 模式固定 IP
    };
    Json(serde_json::json!({ "ip": ip }))
}

async fn wifi_status(State(_s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    if !wifi_available() {
        return Json(serde_json::json!({
            "ssid": null,
            "ip": "127.0.0.1"
        }));
    }
    let status = wpa_cli(&["status"]).await;
    let ssid = parse_connected_ssid(&status);
    let ip = if ssid.is_some() {
        current_wifi_ip().await
    } else {
        "192.168.4.1".to_string()
    };
    Json(serde_json::json!({
        "ssid": ssid,
        "ip": ip
    }))
}

async fn wifi_scan(State(_s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    if !wifi_available() {
        return Json(serde_json::json!({
            "list": [],
            "connected": null,
            "error": "WIFI_UNAVAILABLE_ON_THIS_PLATFORM"
        }));
    }
    if !ensure_wpa_env().await {
        return Json(serde_json::json!({
            "list": [],
            "error": "WPA_INIT_FAILED"
        }));
    }

    wpa_cli(&["scan"]).await;
    tokio::time::sleep(Duration::from_millis(1500)).await;
    let raw = wpa_cli(&["scan_results"]).await;
    let status = wpa_cli(&["status"]).await;
    let connected = parse_connected_ssid(&status);

    // 按 ssid 去重，保留信号最强的那条
    let mut unique: std::collections::BTreeMap<String, serde_json::Value> =
        std::collections::BTreeMap::new();
    for (i, line) in raw.lines().enumerate() {
        if i == 0 {
            continue; // header: bssid / frequency / signal level / flags / ssid
        }
        let parts: Vec<&str> = line.split('\t').collect();
        if parts.len() < 5 {
            continue;
        }
        let ssid_raw = parts[4].trim();
        if ssid_raw.is_empty() {
            continue;
        }
        let ssid = decode_ssid(ssid_raw);
        let Ok(signal) = parts[2].parse::<i32>() else { continue };
        let flags = parts[3];
        let secured = !(flags == "[ESS]" || flags == "[WPS][ESS]");
        // base64(ssid) 去 padding 当前端 id
        let id = base64::engine::general_purpose::STANDARD
            .encode(&ssid)
            .replace('=', "");
        let is_connected = connected.as_deref() == Some(&ssid);

        let entry = serde_json::json!({
            "ssid": ssid,
            "id": id,
            "signal": signal,
            "secured": secured,
            "is_connected": is_connected,
        });
        match unique.get(&ssid) {
            Some(prev) => {
                let prev_signal = prev["signal"].as_i64().unwrap_or(i64::MIN);
                if signal as i64 > prev_signal {
                    unique.insert(ssid.to_string(), entry);
                }
            }
            None => {
                unique.insert(ssid.to_string(), entry);
            }
        }
    }

    let mut list: Vec<serde_json::Value> = unique.into_values().collect();
    // 排序：已连接的在前，其余按信号强度倒序
    list.sort_by(|a, b| {
        let ac = a["is_connected"].as_bool().unwrap_or(false);
        let bc = b["is_connected"].as_bool().unwrap_or(false);
        match (ac, bc) {
            (true, false) => std::cmp::Ordering::Less,
            (false, true) => std::cmp::Ordering::Greater,
            _ => {
                let sa = a["signal"].as_i64().unwrap_or(0);
                let sb = b["signal"].as_i64().unwrap_or(0);
                sb.cmp(&sa)
            }
        }
    });

    Json(serde_json::json!({
        "list": list,
        "connected": connected,
    }))
}

#[derive(Deserialize)]
struct ConnectBody {
    #[serde(default)]
    ssid: String,
    #[serde(default)]
    password: String,
}

/// POST /connect 返回纯文本 "success|..." / "error|..."，
/// 前端按 '|' split 成两段（不要返回 JSON，会破坏前端 parse）。
async fn wifi_connect(
    State(_s): State<Arc<AppState>>,
    Json(body): Json<ConnectBody>,
) -> impl IntoResponse {
    if !wifi_available() {
        return (
            StatusCode::OK,
            "error|WiFi 在当前平台不可用（需 Linux + wpa_supplicant）",
        )
            .into_response();
    }

    let ssid = body.ssid.trim();
    let password = body.password.as_str();
    if ssid.is_empty() {
        return (StatusCode::OK, "error|SSID 不能为空").into_response();
    }

    if !ensure_wpa_env().await {
        return (StatusCode::OK, "error|wpa_supplicant 启动失败").into_response();
    }

    // 收集每步 wpa_cli 输出，超时时一起回，便于排错
    let mut diag = String::new();
    let mut append_diag = |s: &str| {
        diag.push_str(s);
        diag.push('\n');
    };

    // 清掉旧 network
    let rm_resp = wpa_cli(&["remove_network", "all"]).await;
    append_diag(&format!("[1] remove_network all -> {:?} (raw bytes len={})",
        rm_resp.trim_end(), rm_resp.len()));

    let add_out = wpa_cli(&["add_network"]).await;
    append_diag(&format!("[2] add_network -> {:?} (raw bytes len={})",
        add_out, add_out.len()));

    // wpa_cli add_network 成功输出形如 "1\nOK"（第一行是新网络 ID，第二行是 OK）。
    // 之前取 last line 拿到 "OK"，导致后续 set_network OK ... 全部失败、select_network
    // 走到 INACTIVE。改成取 first line，并校验是数字。
    // 同时 strip \r（wpa_cli 偶尔 CRLF）+ trim 防御。
    let net_id = add_out
        .lines()
        .next()
        .map(|s| s.trim().trim_end_matches('\r').to_string())
        .unwrap_or_default();
    if net_id.is_empty() || !net_id.chars().all(|c| c.is_ascii_digit()) {
        return (
            StatusCode::OK,
            format!("error|add_network 返回无效 net_id={net_id:?}, wpa 原始输出={add_out:?}"),
        )
            .into_response();
    }
    append_diag(&format!("[3] net_id = {net_id}"));

    // ⚠️ 关键：每个 token 必须作为独立 argv entry。
    // busybox wpa_cli 不 tokenize argv，整个 argv[1] 当命令名，
    // 所以 "set_network 0 ssid \"X\"" 当一个 argv 传会被 busybox 当成未知命令。
    // Full wpa_supplicant 自带的 wpa_cli 会自己 join + tokenize，所以两种都 OK。
    // 我们按独立 argv 走，兼容两边。
    // 中文 SSID 用 hex 编码传给 wpa_cli
    let ssid_hex: String = ssid.as_bytes().iter().map(|b| format!("{:02x}", b)).collect();
    let ssid_resp = wpa_cli(&[
        "set_network",
        &net_id,
        "ssid",
        &format!("\"{ssid_hex}\""),
    ]).await;
    append_diag(&format!(
        "[4] set_network ssid argv=[set_network, {net_id:?}, ssid, \"{ssid}\"] -> {ssid_resp:?}"
    ));
    if ssid_resp.trim() != "OK" {
        return (
            StatusCode::OK,
            format!("error|set_network ssid 失败: {ssid_resp}\ndiag:\n{diag}"),
        )
            .into_response();
    }

    if !password.is_empty() {
        let psk_resp = wpa_cli(&[
            "set_network",
            &net_id,
            "psk",
            &format!("\"{password}\""),
        ]).await;
        append_diag(&format!(
            "[5] set_network psk argv=[set_network, {net_id:?}, psk, \"***\"] -> {psk_resp:?}"
        ));
        if psk_resp.trim() != "OK" {
            return (
                StatusCode::OK,
                format!("error|set_network psk 失败: {psk_resp}\ndiag:\n{diag}"),
            )
                .into_response();
        }
    } else {
        let none_resp = wpa_cli(&[
            "set_network",
            &net_id,
            "key_mgmt",
            "NONE",
        ]).await;
        append_diag(&format!(
            "[5] set_network key_mgmt NONE argv=[set_network, {net_id:?}, key_mgmt, NONE] -> {none_resp:?}"
        ));
        if none_resp.trim() != "OK" {
            return (
                StatusCode::OK,
                format!("error|set_network key_mgmt NONE 失败: {none_resp}\ndiag:\n{diag}"),
            )
                .into_response();
        }
    }

    let select_resp = wpa_cli(&["select_network", &net_id]).await;
    append_diag(&format!(
        "[6] select_network argv=[select_network, {net_id:?}] -> {select_resp:?}"
    ));
    if select_resp.trim() != "OK" {
        return (
            StatusCode::OK,
            format!("error|select_network 失败: {select_resp}\ndiag:\n{diag}"),
        )
            .into_response();
    }

    // 等连接完成，最多 10 轮。认证失败 / AP 不在范围立刻返回。
    let mut last_state = String::new();
    for attempt in 0..10 {
        tokio::time::sleep(Duration::from_millis(800)).await;
        let status = wpa_cli(&["status"]).await;
        // 抓 wpa_state
        for line in status.lines() {
            if let Some(s) = line.strip_prefix("wpa_state=") {
                last_state = s.to_string();
                break;
            }
        }
        if status.contains("wpa_state=COMPLETED") {
            cmd_run(&["udhcpc", "-i", WIFI_INTERFACE, "-n", "-q", "-T", "3"]).await;
            let ip = current_wifi_ip().await;
            return (
                StatusCode::OK,
                format!("success|连接成功! IP: {}", if ip.is_empty() { "未知" } else { &ip }),
            )
                .into_response();
        }
        // 认证失败 / AP 不可达 — 快速返回
        if last_state == "DISCONNECTED" || last_state == "INACTIVE"
            || status.contains("FAIL") || status.contains("UNKNOWN")
            || status.contains("reason=WRONG_KEY")
        {
            return (
                StatusCode::OK,
                format!("error|连接失败（{}），请检查密码或信号\ndiag:\n{}", last_state, diag),
            )
                .into_response();
        }
        // 一直在扫描 — AP 不在范围
        if attempt >= 2 && last_state == "SCANNING" {
            return (
                StatusCode::OK,
                "error|未找到该网络".to_string(),
            )
                .into_response();
        }
    }

    (
        StatusCode::OK,
        format!(
            "error|连接超时（10s），最后状态 wpa_state={}\ndiag:\n{}",
            if last_state.is_empty() { "UNKNOWN" } else { &last_state },
            diag
        ),
    )
        .into_response()
}