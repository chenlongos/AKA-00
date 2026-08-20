//! 控制路由 —— REST + WebSocket，匹配前端 ControlSocket 和 api.ts 契约
//!
//! 对应 `run.py` 中的 ControlWebSocket 和 `app/routes/api.py`。

use axum::{
    extract::{
        ws::{Message, WebSocket},
        Query, State, WebSocketUpgrade,
    },
    response::IntoResponse,
    Json, Router,
};
use axum::routing::get;
use futures_util::{SinkExt, StreamExt};
use serde::Deserialize;
use std::sync::Arc;
use tokio::sync::Mutex as TokioMutex;
use tokio::time::{interval, Duration};

use crate::services::motor::Action;
use crate::AppState;

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/control", get(control_action))
        .route("/ws/control", get(ws_handler))
}

// ── REST: GET /api/control?action=up|down|left|right|stop|grab|release&speed=N&distance=N&angle=N&time=N ──
// speed: 电机百分比 (1~100, default 50), distance: cm, angle: degrees

const WHEEL_BASE_M: f32 = 0.15;

fn pct_to_motor(pct: f32) -> u8 {
    pct.clamp(1.0, 100.0) as u8
}

#[derive(Deserialize, Default)]
struct ControlParams {
    action: Option<String>,
    speed: Option<f32>,
    distance: Option<f32>,
    angle: Option<f32>,
    #[serde(rename = "time")]
    duration: Option<u32>,
}

async fn control_action(
    State(s): State<Arc<AppState>>,
    Query(p): Query<ControlParams>,
) -> Json<serde_json::Value> {
    let action_str = p.action.as_deref().unwrap_or("");
    if action_str == "grab" || action_str == "release" {
        let payload = serde_json::json!({ "command": action_str });
        s.arm.handle_json_cmd(&payload).await;
        return Json(serde_json::json!({"ok": true, "via": "arm"}));
    }

    let action = match action_str {
        "up" => Action::Up,
        "down" => Action::Down,
        "left" => Action::Left,
        "right" => Action::Right,
        "stop" => Action::Stop,
        _ => { return Json(serde_json::json!({"error": "unknown action"})); }
    };

    let motor_speed = pct_to_motor(p.speed.unwrap_or(50.0));

    if let Some(dist_cm) = p.distance {
        // 闭环距离控制
        s.motor.move_distance(action_str, dist_cm * 10.0, motor_speed).await;
        return Json(serde_json::json!({"ok": true, "action": action_str, "mode": "closed_loop", "distance_mm": dist_cm * 10.0}));
    }
    if let Some(angle_deg) = p.angle {
        // 转角 → 估算距离（弧长）
        let arc_mm = (angle_deg / 360.0) * std::f32::consts::PI * WHEEL_BASE_M * 1000.0;
        let dir = match action_str { "left" => "left", "right" => "right", _ => action_str };
        s.motor.move_distance(dir, arc_mm, motor_speed).await;
        return Json(serde_json::json!({"ok": true, "action": action_str, "mode": "closed_loop", "angle_deg": angle_deg}));
    }

    // 无 distance/angle → 开环时间控制
    let duration_ms = p.duration.unwrap_or(0);
    s.motor.action(action, motor_speed, duration_ms).await;
    Json(serde_json::json!({"ok": true, "action": action_str, "mode": "open_loop", "duration_ms": duration_ms}))
}

// ── WebSocket: /ws/control ──
// 二进制协议：
//   Client→Server: 0xAA x(int8) y(int8)  — 摇杆
//   Client→Server: 0xDD JSON...           — JSON 命令
//   Server→Client: 0xBB left(int16) right(int16) — 电机状态（每200ms）
//   Server→Client: 0xDD JSON...           — JSON 响应（开场白 = {"type":"ip", "ip":"..."}）

async fn ws_handler(ws: WebSocketUpgrade, State(s): State<Arc<AppState>>) -> impl IntoResponse {
    let motor = s.motor.clone();
    let arm = s.arm.clone();
    ws.on_upgrade(move |socket| handle_ws(socket, motor, arm))
}

/// 读指定网卡的 IPv4 地址。对应 Python `get_ip(iface)`（fcntl ioctl SIOCGIFADDR）。
/// 这里用 `ip` 命令更简单（板子上必有），输出形如：
///   "4: wlan0    inet 192.168.4.1/24 brd 192.168.4.255 scope global wlan0\n"
/// 找 "inet " 后面那个 CIDR，strip /xx。
///
/// 这两个函数跑在 spawn_blocking 里（不阻塞 tokio reactor），所以即使是
/// sync Command 也安全。
fn get_iface_ip_blocking(iface: &str) -> String {
    use std::process::Command;
    let Ok(out) = Command::new("ip")
        .args(["-4", "-o", "addr", "show", iface])
        .output()
    else {
        return String::new();
    };
    if !out.status.success() {
        return String::new();
    }
    let s = String::from_utf8_lossy(&out.stdout);
    for line in s.lines() {
        let parts: Vec<&str> = line.split_whitespace().collect();
        for (i, p) in parts.iter().enumerate() {
            if *p == "inet" && i + 1 < parts.len() {
                if let Some(addr) = parts[i + 1].split('/').next() {
                    if !addr.is_empty() {
                        return addr.to_string();
                    }
                }
            }
        }
    }
    String::new()
}

async fn get_iface_ip(iface: &str) -> String {
    let iface = iface.to_string();
    tokio::task::spawn_blocking(move || get_iface_ip_blocking(&iface))
        .await
        .unwrap_or_default()
}

/// UDP 探测拿出口网卡 IP（连公网时这是 wlan1 的 IP，AP 模式下探测失败）
async fn udp_probe_ip() -> Option<String> {
    tokio::task::spawn_blocking(|| {
        use std::net::UdpSocket;
        let sock = UdpSocket::bind("0.0.0.0:0").ok()?;
        sock.connect("8.8.8.8:80").ok()?;
        let addr = sock.local_addr().ok()?;
        let ip = addr.ip().to_string();
        if ip.is_empty() || ip == "0.0.0.0" {
            None
        } else {
            Some(ip)
        }
    })
    .await
    .ok()
    .flatten()
}

/// 取本机 IP（用于 ws 开场白 + `/api/system/ip` + WS `{type:"ip"}` 响应）。
///
/// 优先级（每次都重跑，不缓存，方便 wifi 切换后立刻反映新 IP）：
///   1. UDP 探测 8.8.8.8（连公网时直接拿到 wlan1 出口 IP）
///   2. wlan1 静态 IP（连内网 wifi / 还没拿到 DHCP 时）
///   3. wlan0 静态 IP（AP 模式默认 192.168.4.1）
///   4. 127.0.0.1（兜底）
///
/// 对应 Python `app/routes/_utils.py::get_wifi_ip`：wlan1 → wlan0 → hostname，
/// 但 UDP 探测先试（如果有公网，立刻拿到真出口 IP，wlan0 / wlan1 静态 IP
/// 反而是次优）。
pub async fn detect_local_ip() -> String {
    // 1. UDP 探测（最准：直接告诉前端"你现在能被外网看到的 IP"）
    if let Some(ip) = udp_probe_ip().await {
        return ip;
    }
    // 2. wlan1 静态 IP（内网 wifi 或 AP 模式）
    let wlan1 = get_iface_ip("wlan1").await;
    if !wlan1.is_empty() {
        return wlan1;
    }
    // 3. wlan0 静态 IP（AP 默认 192.168.4.1）
    let wlan0 = get_iface_ip("wlan0").await;
    if !wlan0.is_empty() {
        return wlan0;
    }
    "127.0.0.1".to_string()
}

async fn handle_ws(
    socket: WebSocket,
    motor: Arc<crate::services::motor::MotorService>,
    arm: Arc<crate::services::arm::ArmService>,
) {
    log::info!("[ws] client connected");

    let (tx, mut rx) = socket.split();
    // SplitSink 不支持 Clone，但 send_task 和 receive loop 都要发消息。
    // 用 Arc<TokioMutex<>> 共享：偶尔发，不锁竞争。
    let tx = Arc::new(TokioMutex::new(tx));
    let motor_tx = motor.clone();

    // 开场白：触发前端 wsReady 状态机（BaseControlPage.tsx:35-39）
    // 不发这一条，前端卡在 wsReady=false，init useEffect 不跑，
    // sendPwmChannels / sendReinitialize / hash-routing 全都失效。
    // IP 走 UDP 探测（async + spawn_blocking），不阻塞 tokio reactor。
    let local_ip = detect_local_ip().await;
    let welcome = serde_json::json!({ "type": "ip", "ip": local_ip });
    match serde_json::to_vec(&welcome) {
        Ok(json_bytes) => {
            let mut buf = Vec::with_capacity(1 + json_bytes.len());
            buf.push(0xDD);
            buf.extend_from_slice(&json_bytes);
            let send_res = tx.lock().await.send(Message::Binary(buf.into())).await;
            if send_res.is_err() {
                log::warn!("[ws] failed to send welcome message");
            } else {
                log::info!("[ws] welcome sent (ip={})", local_ip);
            }
        }
        Err(e) => log::warn!("[ws] serialize welcome: {:?}", e),
    }

    // 发送任务：每 200ms 推送电机状态（左/右轮线速度 m/s）
    // send_task 用一份 Arc clone（receive loop 还要用 tx 发 ip 响应）
    let tx_for_send = tx.clone();
    let send_task = tokio::spawn(async move {
        let mut tick = interval(Duration::from_millis(200));
        loop {
            tick.tick().await;
            let s = motor_tx.status();
            let mut buf = vec![0xBBu8];
            // 前端 (api.ts:125) 把 int16 当作 mm/s 读（再除以 1000 得 m/s）
            // 这里把 float m/s × 1000 编码成 int16 mm/s，精度 1mm/s
            // LE 对齐前端 api.ts DataView.getInt16(..., true) 和 Python struct.pack("<Bhh", ...)
            let left_mmps = (s.left_speed * 1000.0).round() as i16;
            let right_mmps = (s.right_speed * 1000.0).round() as i16;
            buf.extend_from_slice(&left_mmps.to_le_bytes());
            buf.extend_from_slice(&right_mmps.to_le_bytes());
            if tx_for_send.lock().await.send(Message::Binary(buf.into())).await.is_err() {
                break;
            }
        }
    });

    // 接收任务：解析 joystick / JSON 命令
    while let Some(Ok(msg)) = rx.next().await {
        match msg {
            Message::Binary(data) => {
                if data.is_empty() {
                    continue;
                }
                match data[0] {
                    0xAA if data.len() >= 3 => {
                        let x = data[1] as i8;
                        let y = data[2] as i8;
                        motor.joystick(x, y).await;
                    }
                    0xDD => {
                        if let Ok(cmd) =
                            serde_json::from_slice::<serde_json::Value>(&data[1..])
                        {
                            // WS 消息分流 —— 全部翻译成 service 方法调用，让 service 产出
                            // motor-bridge 期望的 canonical JSON。
                            //
                            // 之前对未知 type 直接 motor.handle_json_cmd 裸转发，
                            // 把前端 {type:"action", speed, time} 这种 WS 风格 JSON
                            // 透给 motor-bridge 会因字段不匹配 (缺 command, time vs
                            // duration) 报 parse error。
                            //
                            // 现在按 type 显式分发：
                            //   arm_cmd              → arm 服务 (展开 payload)
                            //   action (grab/release) → arm 服务
                            //   action (up/down/...) → motor.action() 翻译
                            //   其他                  → log warn 后丢弃
                            let msg_type = cmd
                                .get("type")
                                .and_then(|v| v.as_str())
                                .unwrap_or("");

                            match msg_type {
                                "arm_cmd" => {
                                    let arm = arm.clone();
                                    let payload = cmd
                                        .get("payload")
                                        .cloned()
                                        .unwrap_or(serde_json::Value::Null);
                                    tokio::spawn(async move {
                                        arm.handle_json_cmd(&payload).await;
                                    });
                                }
                                "ip" => {
                                    // 刷新 IP：wifi 切换后前端可以发这条重查，不必断 WS。
                                    // 回 {type:"ip", ip:"..."} 走 0xDD 帧。
                                    let ip_now = detect_local_ip().await;
                                    let resp = serde_json::json!({"type": "ip", "ip": ip_now});
                                    if let Ok(bytes) = serde_json::to_vec(&resp) {
                                        let mut buf = Vec::with_capacity(1 + bytes.len());
                                        buf.push(0xDD);
                                        buf.extend_from_slice(&bytes);
                                        if tx.lock().await.send(Message::Binary(buf.into())).await.is_err() {
                                            log::debug!("[ws] ip refresh send failed");
                                        }
                                    }
                                }
                                "action" => {
                                    let inner = cmd
                                        .get("action")
                                        .and_then(|v| v.as_str())
                                        .unwrap_or("");
                                    let speed = cmd
                                        .get("speed")
                                        .and_then(|v| v.as_u64())
                                        .unwrap_or(50)
                                        as u8;
                                    let duration = cmd
                                        .get("time")
                                        .and_then(|v| v.as_u64())
                                        .unwrap_or(0)
                                        as u32;

                                    match inner {
                                        "grab" | "release" => {
                                            let arm = arm.clone();
                                            let payload = serde_json::json!({
                                                "command": inner,
                                            });
                                            tokio::spawn(async move {
                                                arm.handle_json_cmd(&payload).await;
                                            });
                                        }
                                        "up" => motor.action(Action::Up, speed, duration).await,
                                        "down" => motor.action(Action::Down, speed, duration).await,
                                        "left" => motor.action(Action::Left, speed, duration).await,
                                        "right" => motor.action(Action::Right, speed, duration).await,
                                        "stop" => motor.action(Action::Stop, speed, duration).await,
                                        other => {
                                            log::warn!("[ws] unknown action: {}", other);
                                        }
                                    }
                                }
                                other => {
                                    // raw_command / pwm_channels / reinitialize / ip 等
                                    // 不是手机快速控制用的，统一 warn 提示走 HTTP，
                                    // 避免裸转发到 motor-bridge 又报 parse error。
                                    log::debug!("[ws] ignoring 0xDD type={} (use HTTP for this)", other);
                                }
                            }
                        }
                    }
                    _ => {}
                }
            }
            Message::Close(_) => break,
            _ => {}
        }
    }

    send_task.abort();
    // 断开连接时自动停电机，防止摇杆松手后小车继续跑
    motor.action(Action::Stop, 0, 0).await;
    log::info!("[ws] client disconnected");
}
