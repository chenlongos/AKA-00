//! 控制路由 —— REST + WebSocket，匹配前端 ControlSocket 和 api.ts 契约
//!
//! 对应 `run.py` 中的 ControlWebSocket 和 `app/routes/api.py`。

use std::sync::Arc;

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
use tokio::time::{interval, Duration};

use crate::services::motor::Action;
use crate::AppState;

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/control", get(control_action))
        .route("/ws/control", get(ws_handler))
}

// ── REST: GET /api/control?action=up|down|left|right|stop|grab|release&speed=N&time=N ──

#[derive(Deserialize, Default)]
struct ControlParams {
    action: Option<String>,
    speed: Option<u8>,
    #[serde(rename = "time")]
    duration: Option<u32>,
}

async fn control_action(
    State(s): State<Arc<AppState>>,
    Query(p): Query<ControlParams>,
) -> Json<serde_json::Value> {
    // grab/release 是机械臂高层动作，分流到 ArmService 而不是 MotorService。
    // 之前全部走 motor.path → motor-bridge 收到 {action:"grab"} 后 match 没
    // 这个分支就 _=>continue 静默丢弃；WS 路径更糟 —— 前端包成
    // {type:"action",action:"grab",speed,time}（没 command 字段、time 不是
    // duration）连解析都过不了。对应 Python app/services/control_service.py
    // 的 _apply_arm_action。
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
        _ => {
            return Json(serde_json::json!({"error": "unknown action"}));
        }
    };
    s.motor.action(action, p.speed.unwrap_or(100), p.duration.unwrap_or(0)).await;
    Json(serde_json::json!({"ok": true}))
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
/// 这个是 blocking `Command::output`，但只跑 1-2 次（每次 WS 连接只触发一次），
/// ip 命令一般 5ms 内完成，不会卡住 tokio reactor。生产实现可以换成
/// libc::ioctl(SIOCGIFADDR) 直接读 ifreq，但需要 unsafe + 跨平台兼容代码。
fn get_iface_ip(iface: &str) -> String {
    use std::process::Command;
    let Ok(out) = Command::new("ip")
        .args(["-4", "-o", "addr", "show", iface])
        .output()
    else {
        return String::new();
    };
    if !out.status.success() {
        return String::new(); // 网卡不存在等
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

/// 取本机 IP（用于 ws 开场白里的 `ip` 字段；前端用它跳转 labs.chenlongrobot.com）。
///
/// 对应 Python `app/routes/_utils.py::get_wifi_ip`：wlan1 (STA 已连接) 优先，
/// wlan0 (AP 默认 192.168.4.1) 兜底，最后 127.0.0.1。
///
/// 之前纯 UDP 探测 8.8.8.8，AP-only 模式下 8.8.8.8 没路由 → 拿到 0.0.0.0 → 前端显示空白。
fn detect_local_ip() -> String {
    for iface in ["wlan1", "wlan0"] {
        let ip = get_iface_ip(iface);
        if !ip.is_empty() {
            return ip;
        }
    }
    "127.0.0.1".to_string()
}

async fn handle_ws(
    socket: WebSocket,
    motor: Arc<crate::services::motor::MotorService>,
    arm: Arc<crate::services::arm::ArmService>,
) {
    log::info!("[ws] client connected");

    let (mut tx, mut rx) = socket.split();
    let motor_tx = motor.clone();

    // 开场白：触发前端 wsReady 状态机（BaseControlPage.tsx:35-39）
    // 不发这一条，前端卡在 wsReady=false，init useEffect 不跑，
    // sendPwmChannels / sendReinitialize / hash-routing 全都失效。
    let local_ip = detect_local_ip();
    let welcome = serde_json::json!({ "type": "ip", "ip": local_ip });
    match serde_json::to_vec(&welcome) {
        Ok(json_bytes) => {
            let mut buf = Vec::with_capacity(1 + json_bytes.len());
            buf.push(0xDD);
            buf.extend_from_slice(&json_bytes);
            if tx.send(Message::Binary(buf.into())).await.is_err() {
                log::warn!("[ws] failed to send welcome message");
            } else {
                log::info!("[ws] welcome sent (ip={})", local_ip);
            }
        }
        Err(e) => log::warn!("[ws] serialize welcome: {:?}", e),
    }

    // 发送任务：每 200ms 推送电机状态（左/右轮线速度 m/s）
    let send_task = tokio::spawn(async move {
        let mut tick = interval(Duration::from_millis(200));
        loop {
            tick.tick().await;
            let s = motor_tx.status();
            let mut buf = vec![0xBBu8];
            // 前端 (api.ts:125) 把 int16 当作 mm/s 读（再除以 1000 得 m/s）
            // 这里把 float m/s × 1000 编码成 int16 mm/s，精度 1mm/s
            let left_mmps = (s.left_speed * 1000.0).round() as i16;
            let right_mmps = (s.right_speed * 1000.0).round() as i16;
            buf.extend_from_slice(&left_mmps.to_be_bytes());
            buf.extend_from_slice(&right_mmps.to_be_bytes());
            if tx.send(Message::Binary(buf.into())).await.is_err() {
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
    log::info!("[ws] client disconnected");
}
