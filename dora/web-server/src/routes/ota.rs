//! OTA 固件升级路由 — 下载 aka-00-server，lock + killall + --update

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::Mutex;

use axum::{extract::State, Json, Router};
use axum::routing::{get, post};
use serde::Serialize;

use crate::AppState;

// ── 路径 ──────────────────────────────────────────
fn app_dir() -> PathBuf {
    std::env::var("DORA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/root/AKA-00"))
}
fn version_file() -> PathBuf { app_dir().join("VERSION") }
fn ota_dir() -> PathBuf { app_dir().join(".ota") }
fn status_file() -> PathBuf { PathBuf::from("/root/aka-ota-status.json") }
fn gen_task_id() -> String {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .subsec_nanos();
    format!("{:08x}", nanos)
}
fn lock_file() -> PathBuf { PathBuf::from("/tmp/aka-ota-lock") }

// ── 进度状态（内存，支持多 task） ──────────────────────────────
#[derive(Clone, Default)]
pub struct OtaState {
    pub tasks: Arc<Mutex<std::collections::HashMap<String, OtaProgress>>>,
}

impl OtaState {
    /// 获取或设置某个 task 的进度（缺省返回 idle）
    pub fn get_progress(&self, task_id: &str) -> OtaProgress {
        let map = self.tasks.lock().unwrap();
        map.get(task_id).cloned().unwrap_or(OtaProgress {
            progress: 0,
            status: "unknown".into(),
            message: "".into(),
        })
    }

    /// 设置某个 task 的进度
    pub fn set_progress(&self, task_id: &str, progress: OtaProgress) {
        let mut map = self.tasks.lock().unwrap();
        map.insert(task_id.to_string(), progress);
    }

    /// 查找第一个活跃的 downloading/installing 任务（给 /api/ota/status 用）
    pub fn active_progress(&self) -> Option<OtaProgress> {
        let map = self.tasks.lock().unwrap();
        map.values()
            .find(|p| p.status == "downloading" || p.status == "installing")
            .cloned()
    }
}

#[derive(Clone, Serialize)]
pub struct OtaProgress {
    pub progress: u32,
    pub status: String,
    pub message: String,
}

// ── 版本解析 ──────────────────────────────────────
fn parse_semver(ver: &str) -> Option<(u32, u32, u32, u32)> {
    let s = ver.trim_start_matches('v');
    let main = s.split('-').next()?;
    let parts: Vec<&str> = main.split('.').collect();
    let major = parts.first()?.parse().ok()?;
    let minor = parts.get(1).unwrap_or(&"0").parse().ok()?;
    let patch = parts.get(2).unwrap_or(&"0").parse().ok()?;
    // commit count after tag: "v1.2.3-4-gabc" → 4
    let commits = s.split('-')
        .nth(1)
        .and_then(|p| p.parse().ok())
        .unwrap_or(0);
    Some((major, minor, patch, commits))
}

fn read_version() -> (String, String) {
    let path = version_file();
    if let Ok(s) = std::fs::read_to_string(&path) {
        let raw = s.trim().to_string();
        let sep = if raw.contains('@') { '@' } else { ' ' };
        if let Some((a, b)) = raw.rsplit_once(sep) {
            return (a.to_string(), b.to_string());
        }
        return (raw.clone(), raw);
    }
    ("unknown".into(), "0".into())
}

fn write_ota_status(status: &str, extra: &[(&str, serde_json::Value)]) {
    let mut data = serde_json::json!({
        "status": status,
        "timestamp": std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs(),
    });
    if let Some(obj) = data.as_object_mut() {
        for (k, v) in extra {
            obj.insert(k.to_string(), v.clone());
        }
    }
    if let Ok(s) = serde_json::to_string(&data) {
        let _ = std::fs::write(status_file(), s);
    }
}

fn read_ota_status() -> serde_json::Value {
    std::fs::read_to_string(status_file())
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or(serde_json::json!({"status": "idle"}))
}

// ── 路由注册 ──────────────────────────────────────
#[derive(Serialize)]
struct VersionResponse {
    version: String,
    updated: i64,
    service: String,
}

pub fn router() -> Router<Arc<AppState>> {
    Router::new()
        .route("/api/ota/version", get(get_version))
        .route("/api/ota/check", get(check_update))
        .route("/api/ota/upgrade", post(do_upgrade))
        .route("/api/ota/update", post(do_update))
        .route("/api/ota/status", get(get_ota_status))
        .route("/api/ota/upgrade/progress", get(get_upgrade_progress))
}

async fn get_version(State(_s): State<Arc<AppState>>) -> Json<VersionResponse> {
    let (ver, ts) = read_version();
    Json(VersionResponse {
        version: ver,
        updated: ts.parse().unwrap_or(0),
        service: "AKA-00".into(),
    })
}

async fn get_ota_status(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    // 优先查内存中的活跃任务
    if let Some(progress) = s.ota.active_progress() {
        return Json(serde_json::json!(progress));
    }
    // 再查磁盘
    Json(read_ota_status())
}

async fn check_update(State(_s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    let (cur_ver, cur_ts_str) = read_version();
    let cur_ts: i64 = cur_ts_str.parse().unwrap_or(0);

    let check_url = std::env::var("OTA_CHECK_URL").unwrap_or_default();
    if check_url.is_empty() {
        return Json(serde_json::json!({
            "current_version": cur_ver,
            "current_updated": cur_ts,
            "remote_updated": 0,
            "update_available": false,
            "latest_version": "",
            "hardware_desc": "",
            "software_desc": "",
            "url": "",
        }));
    }

    let (remote_ts, latest_ver, hw_desc, sw_desc, url) =
        fetch_remote(&check_url).await.unwrap_or_default();

    let lv = parse_semver(&cur_ver);
    let rv = parse_semver(&latest_ver);

    let has_update = if lv.is_some() && rv.is_some() {
        rv > lv
    } else {
        remote_ts > cur_ts
    };

    Json(serde_json::json!({
        "current_version": cur_ver,
        "current_updated": cur_ts,
        "remote_updated": remote_ts,
        "update_available": has_update,
        "latest_version": latest_ver,
        "hardware_desc": hw_desc,
        "software_desc": sw_desc,
        "url": url,
    }))
}

async fn do_upgrade(State(s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    let check_url = std::env::var("OTA_CHECK_URL").unwrap_or_default();
    if check_url.is_empty() {
        return Json(serde_json::json!({"status": "error", "message": "未配置 OTA_CHECK_URL"}));
    }

    let (_, _, _, _, url) = match fetch_remote(&check_url).await {
        Ok(r) => r,
        _ => return Json(serde_json::json!({"status": "error", "message": "无法获取更新信息"})),
    };

    if url.is_empty() {
        return Json(serde_json::json!({"status": "error", "message": "未找到下载地址"}));
    }

    // 生成 task_id
    let task_id = gen_task_id();

    s.ota.set_progress(&task_id, OtaProgress { progress: 0, status: "downloading".into(), message: "准备下载...".into() });
    write_ota_status("downloading", &[]);

    let state = s.ota.clone();
    let tid = task_id.clone();
    tokio::spawn(async move {
        let _ = do_download_and_install(&url, &tid, &state).await;
    });

    Json(serde_json::json!({"status": "ok", "task_id": task_id}))
}

async fn do_download_and_install(
    url: &str,
    task_id: &str,
    state: &OtaState,
) -> Result<(), String> {
    state.set_progress(task_id, OtaProgress { progress: 0, status: "downloading".into(), message: "正在下载固件...".into() });

    let client = reqwest::Client::new();
    let mut resp = client.get(url).send().await.map_err(|e| e.to_string())?;
    let total = resp.content_length().unwrap_or(0);
    let mut downloaded: u64 = 0;
    let mut bytes = Vec::new();

    loop {
        let chunk = match resp.chunk().await {
            Ok(Some(c)) => c,
            Ok(None) => break,
            Err(e) => return Err(format!("download chunk: {e}")),
        };
        downloaded += chunk.len() as u64;
        bytes.extend_from_slice(&chunk);
        if total > 0 {
            let pct = (downloaded * 100 / total).min(99) as u32;
            state.set_progress(task_id, OtaProgress {
                progress: pct,
                status: "downloading".into(),
                message: format!("正在下载... {}%", pct),
            });
        }
    }

    // 写入 /tmp/aka-ota-update
    let update_path = PathBuf::from("/tmp/aka-ota-update");
    std::fs::write(&update_path, &bytes).map_err(|e| e.to_string())?;
    #[cfg(unix)]
    { use std::os::unix::fs::PermissionsExt; let _ = std::fs::set_permissions(&update_path, std::fs::Permissions::from_mode(0o755)); }

    write_ota_status("installing", &[]);
    state.set_progress(task_id, OtaProgress { progress: 100, status: "installing".into(), message: "正在安装...".into() });

    // 写 install 脚本
    write_install_script();
    state.set_progress(task_id, OtaProgress { progress: 100, status: "done".into(), message: "安装完成，服务重启中...".into() });

    Ok(())
}

fn write_install_script() {
    let script = r#"#!/bin/sh
set -e
LOCK_FILE="/tmp/aka-ota-lock"

# Give HTTP response time to flush
sleep 3

echo "[OTA] acquiring lock..."
touch "$LOCK_FILE"

echo "[OTA] stopping old process..."
killall web-server 2>/dev/null || true
sleep 2
killall -9 web-server 2>/dev/null || true

echo "[OTA] running update..."
exec /tmp/aka-ota-update --update
"#;

    let path = PathBuf::from("/tmp/aka-ota-install.sh");
    let _ = std::fs::write(&path, script);
    #[cfg(unix)]
    { use std::os::unix::fs::PermissionsExt; let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755)); }

    let _ = std::process::Command::new("/bin/sh")
        .arg(&path)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn();
}

// ── /api/ota/upgrade/progress?task_id=... ──
async fn get_upgrade_progress(
    State(s): State<Arc<AppState>>,
    axum::extract::Query(params): axum::extract::Query<std::collections::HashMap<String, String>>,
) -> Json<serde_json::Value> {
    let task_id = params.get("task_id").map(String::as_str).unwrap_or("");
    Json(serde_json::json!(s.ota.get_progress(task_id)))
}

// ── /api/ota/update (POST multipart file upload) ──
async fn do_update(
    State(s): State<Arc<AppState>>,
    mut multipart: axum::extract::Multipart,
) -> Json<serde_json::Value> {
    let task_id = gen_task_id();
    let update_path = PathBuf::from("/tmp/aka-ota-update");

    let mut saved = false;
    while let Ok(Some(field)) = multipart.next_field().await {
        let name = field.name().unwrap_or("").to_string();
        if name == "firmware" {
            let data = match field.bytes().await {
                Ok(b) => b,
                Err(e) => return Json(serde_json::json!({"status": "error", "message": format!("read upload: {e}")})),
            };
            std::fs::write(&update_path, &data).ok();
            #[cfg(unix)]
            { use std::os::unix::fs::PermissionsExt; let _ = std::fs::set_permissions(&update_path, std::fs::Permissions::from_mode(0o755)); }
            saved = true;
            break;
        }
    }

    if !saved {
        return Json(serde_json::json!({"status": "error", "message": "no firmware file"}));
    }

    s.ota.set_progress(&task_id, OtaProgress { progress: 50, status: "installing".into(), message: "正在安装固件...".into() });
    write_ota_status("installing", &[("task_id", serde_json::json!(&task_id))]);

    let state = s.ota.clone();
    let tid = task_id.clone();
    tokio::spawn(async move {
        write_install_script();
        state.set_progress(&tid, OtaProgress { progress: 100, status: "done".into(), message: "安装完成，服务重启中...".into() });
    });

    Json(serde_json::json!({"status": "ok", "task_id": task_id, "message": "upload received, installing..."}))
}

async fn fetch_remote(url: &str) -> Result<(i64, String, String, String, String), String> {
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(5))
        .build()
        .map_err(|e| e.to_string())?;

    let mut resp = client.get(url).send().await.map_err(|e| e.to_string())?;
    let data: serde_json::Value = resp.json().await.map_err(|e| e.to_string())?;

    let inner = data.get("data").unwrap_or(&data);
    let url = inner.get("imageUrl").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let latest = inner.get("versionNumber").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let hw = inner.get("hardwareDesc").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let sw = inner.get("softwareDesc").and_then(|v| v.as_str()).unwrap_or("").to_string();

    let ts = inner.get("updatedAt")
        .and_then(|v| v.as_str())
        .map(|s| {
            let s = s.replace('Z', "").split('.').next().unwrap_or("").to_string();
            chrono::NaiveDateTime::parse_from_str(&s, "%Y-%m-%dT%H:%M:%S")
                .map(|dt| dt.and_utc().timestamp())
                .unwrap_or(0)
        })
        .unwrap_or(0);

    Ok((ts, latest, hw, sw, url))
}
