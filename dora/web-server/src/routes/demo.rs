//! demo HTTP 路由 —— 列表 / 启停（demo-node 转发）
//!
//! 端点：
//!   GET  /api/demo/list                → {demos:[{name,path,kind}]}
//!   POST /api/demo/init  body {name}    发 demo_cmd start 到 demo-node
//!   POST /api/demo/stop                 发 demo_cmd stop 到 demo-node

use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use axum::{
    extract::{Multipart, Path, State},
    Json, Router,
};
use axum::routing::{get, post};
use serde::{Deserialize, Serialize};

use crate::AppState;

// ── 下载进度追踪 ──
pub type DownloadProgressMap = Arc<Mutex<HashMap<String, DownloadTask>>>;

#[derive(Clone, Serialize)]
pub struct DownloadTask {
    progress: u32,
    status: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/demo/list", get(list))
        .route("/api/demo/name", get(demo_name))
        .route("/api/demo/init", post(init_demo))
        .route("/api/demo/stop", post(stop_demo))
        .route("/api/demo/download_model_with_progress", post(download_model))
        .route("/api/demo/upload_model", post(upload_model))
        .route("/api/demo/download_progress/{task_id}", get(download_progress))
}

async fn list(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    Json(serde_json::json!({ "demos": s.demo.list() }))
}

#[derive(Deserialize)]
struct InitBody {
    name: String,
}

async fn init_demo(
    State(s): State<Arc<AppState>>,
    Json(body): Json<InitBody>,
) -> (axum::http::StatusCode, Json<serde_json::Value>) {
    // ── 防御性清理 ──
    // 1. NPU 残留：上次的 stop 可能漏杀 NPU / CVitek TPU worker（demo-node 的
    //    200ms 升级 SIGKILL 用 kill(pid,0) 检查，只看 tennis 主进程，NPU
    //    worker 残留时漏发）。NPU device 一次只能被一个进程 open，残留的
    //    worker 会让新 demo 卡 D-state，表现为"无法停止"。
    //    pkill 扫 tennis / yolo_model / cviruntime 三个特征串，宁错杀。
    // 2. camera preview 还开着：camera-node 持 /dev/videoX → demo open 同一个
    //    → uvcvideo 内核 USB 带宽锁互斥 → demo 进 D-state（uninterruptible
    //    sleep）→ 任何 SIGTERM/SIGKILL 都排不上调度 → 板子整体僵死。
    log::info!("[demo-route] pre-start cleanup for demo '{}'", body.name);
    let _ = std::process::Command::new("pkill")
        .args(["-9", "-f", "tennis|yolo_model.cvimodel|cviruntime"])
        .output();
    // 给 kernel 时间回收 fd / TPU device
    tokio::time::sleep(Duration::from_millis(200)).await;

    if s.camera.is_active() {
        log::info!(
            "[demo-route] preview is on, closing camera before demo '{}' to avoid V4L2 deadlock",
            body.name
        );
        if let Err(e) = s.camera.close().await {
            return (
                axum::http::StatusCode::SERVICE_UNAVAILABLE,
                Json(serde_json::json!({
                    "error": format!("failed to close preview: {e}"),
                })),
            );
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }

    match s.demo.start(&body.name).await {
        Ok(()) => (
            axum::http::StatusCode::OK,
            Json(serde_json::json!({"status": "started", "name": body.name})),
        ),
        Err(e) => (
            axum::http::StatusCode::INTERNAL_SERVER_ERROR,
            Json(serde_json::json!({"error": e.to_string()})),
        ),
    }
}

async fn stop_demo(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    match s.demo.stop().await {
        Ok(()) => Json(serde_json::json!({"status": "stopped"})),
        Err(e) => Json(serde_json::json!({"error": e.to_string()})),
    }
}

// ── GET /api/demo/name ──
async fn demo_name(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    Json(serde_json::json!({ "name": s.demo.current_demo_name() }))
}

// ── GET /api/demo/download_progress/{task_id} ──
async fn download_progress(
    State(s): State<Arc<AppState>>,
    Path(task_id): Path<String>,
) -> Json<serde_json::Value> {
    let map = s.demo_downloads.lock().unwrap();
    let task = map.get(&task_id).cloned().unwrap_or(DownloadTask {
        progress: 0,
        status: "not_found".into(),
        error: None,
    });
    Json(serde_json::json!(task))
}

// ── POST /api/demo/upload_model ──
async fn upload_model(
    State(s): State<Arc<AppState>>,
    mut multipart: Multipart,
) -> (axum::http::StatusCode, Json<serde_json::Value>) {
    let current = s.demo.current_demo_name();
    if current.is_empty() {
        return (axum::http::StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "no local demo found"})));
    }

    let mut saved = false;
    let mut file_size: u64 = 0;
    while let Ok(Some(field)) = multipart.next_field().await {
        let name = field.name().unwrap_or("").to_string();
        if name == "file" {
            let data = match field.bytes().await {
                Ok(b) => b,
                Err(e) => return (axum::http::StatusCode::INTERNAL_SERVER_ERROR,
                    Json(serde_json::json!({"error": format!("read upload: {e}")}))),
            };
            file_size = data.len() as u64;
            let file_path = s.demo.base_dir().join(&current).join("yolo_model.cvimodel");
            if let Some(parent) = file_path.parent() {
                let _ = std::fs::create_dir_all(parent);
            }
            if let Err(e) = std::fs::write(&file_path, &data) {
                return (axum::http::StatusCode::INTERNAL_SERVER_ERROR,
                    Json(serde_json::json!({"error": format!("save file: {e}")})));
            }
            saved = true;
            break;
        }
    }

    if !saved {
        return (axum::http::StatusCode::BAD_REQUEST,
            Json(serde_json::json!({"error": "file is required"})));
    }

    (axum::http::StatusCode::OK,
        Json(serde_json::json!({"status": "uploaded", "size": file_size, "name": current})))
}

// ── POST /api/demo/download_model_with_progress ──
#[derive(Deserialize)]
struct DownloadModelBody {
    model_name: String,
    #[serde(default)]
    demo_server: String,
}

async fn download_model(
    State(s): State<Arc<AppState>>,
    Json(body): Json<DownloadModelBody>,
) -> (axum::http::StatusCode, Json<serde_json::Value>) {
    if body.model_name.is_empty() {
        return (axum::http::StatusCode::BAD_REQUEST,
            Json(serde_json::json!({"error": "model_name is required"})));
    }

    let current = s.demo.current_demo_name();
    if current.is_empty() {
        return (axum::http::StatusCode::NOT_FOUND,
            Json(serde_json::json!({"error": "no local demo found"})));
    }

    let demo_server = if body.demo_server.is_empty() {
        "http://127.0.0.1:9000".to_string()
    } else {
        body.demo_server.clone()
    };
    let url = format!("{}/api/models/{}", demo_server.trim_end_matches('/'), body.model_name);

    let task_id = format!("{}_to_{}", current, body.model_name);

    // 防止无限增长
    {
        let mut map = s.demo_downloads.lock().unwrap();
        if map.len() > 20 {
            map.clear();
        }
        map.insert(task_id.clone(), DownloadTask { progress: 0, status: "downloading".into(), error: None });
    }

    let downloads = s.demo_downloads.clone();
    let demo = s.demo.clone();
    let new_name = body.model_name.clone();
    let tid = task_id.clone();
    tokio::spawn(async move {
        do_download(&tid, &url, &new_name, &demo, &downloads).await;
    });

    (axum::http::StatusCode::OK,
        Json(serde_json::json!({"status": "started", "task_id": task_id, "new_name": body.model_name})))
}

async fn do_download(
    task_id: &str,
    url: &str,
    new_name: &str,
    demo: &Arc<crate::services::demo::DemoService>,
    downloads: &DownloadProgressMap,
) {
    let update = |p: u32, st: &str, err: Option<&str>| {
        let mut map = downloads.lock().unwrap();
        map.insert(task_id.to_string(), DownloadTask {
            progress: p,
            status: st.into(),
            error: err.map(|s| s.to_string()),
        });
    };

    match download_to_file(url, demo, new_name, task_id, downloads).await {
        Ok(()) => update(100, "done", None),
        Err(e) => update(0, "error", Some(&e)),
    }

    // 60 秒后清理
    let tid = task_id.to_string();
    let dl = downloads.clone();
    tokio::spawn(async move {
        tokio::time::sleep(Duration::from_secs(60)).await;
        dl.lock().unwrap().remove(&tid);
    });
}

async fn download_to_file(
    url: &str,
    demo: &crate::services::demo::DemoService,
    new_name: &str,
    task_id: &str,
    downloads: &DownloadProgressMap,
) -> Result<(), String> {
    let client = reqwest::Client::new();
    let mut resp = client.get(url).send().await.map_err(|e| format!("fetch: {e}"))?;
    let total = resp.content_length().unwrap_or(0);
    let mut downloaded: u64 = 0;
    let mut bytes = Vec::new();

    // 重命名目录（如果需要）
    let current = demo.current_demo_name();
    if new_name != current && !current.is_empty() {
        let base_dir = demo.base_dir();
        let old_dir = base_dir.join(&current);
        let new_dir = base_dir.join(new_name);
        if new_dir.exists() {
            let _ = std::fs::remove_dir_all(&new_dir);
        }
        if old_dir.exists() {
            let _ = std::fs::rename(&old_dir, &new_dir);
        }
    }

    let file_path = demo.base_dir().join(new_name).join("yolo_model.cvimodel");
    loop {
        let chunk = match resp.chunk().await {
            Ok(Some(c)) => c,
            Ok(None) => break,
            Err(e) => return Err(format!("download: {e}")),
        };
        downloaded += chunk.len() as u64;
        bytes.extend_from_slice(&chunk);
        if total > 0 {
            let pct = (downloaded * 100 / total).min(99) as u32;
            let mut map = downloads.lock().unwrap();
            map.insert(task_id.to_string(), DownloadTask {
                progress: pct,
                status: "downloading".into(),
                error: None,
            });
        }
    }

    if let Some(parent) = file_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    std::fs::write(&file_path, &bytes).map_err(|e| format!("write: {e}"))?;
    Ok(())
}