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
    let action = match p.action.as_deref() {
        Some("up") => Action::Up,
        Some("down") => Action::Down,
        Some("left") => Action::Left,
        Some("right") => Action::Right,
        Some("stop") => Action::Stop,
        Some("grab") => Action::Grab,
        Some("release") => Action::Release,
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
    ws.on_upgrade(move |socket| handle_ws(socket, motor))
}

/// 取本机 IP（用于 ws 开场白里的 `ip` 字段；前端用它跳转 labs.chenlongrobot.com）。
/// 通过 UDP connect 探测拿到出口网卡 IP，不真正发包。失败则回落到 0.0.0.0。
fn detect_local_ip() -> String {
    use std::net::UdpSocket;
    UdpSocket::bind("0.0.0.0:0")
        .and_then(|s| s.connect("8.8.8.8:80").map(|_| s))
        .ok()
        .and_then(|s| s.local_addr().ok())
        .map(|a| a.ip().to_string())
        .unwrap_or_else(|| "0.0.0.0".to_string())
}

async fn handle_ws(socket: WebSocket, motor: Arc<crate::services::motor::MotorService>) {
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
                            motor.handle_json_cmd(&cmd).await;
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
