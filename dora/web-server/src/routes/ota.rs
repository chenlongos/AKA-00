//! OTA 固件升级路由 — 对应 Python app/routes/ota.py

use std::path::PathBuf;
use std::sync::Arc;

use axum::{extract::State, Json, Router};
use axum::routing::{get, post};
use serde::Serialize;

use crate::AppState;

fn app_dir() -> PathBuf {
    std::env::var("DORA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("/root/AKA-00"))
}

fn version_file() -> PathBuf { app_dir().join("VERSION") }
fn ota_dir() -> PathBuf { app_dir().join(".ota") }

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
}

async fn get_version(State(_s): State<Arc<AppState>>) -> Json<VersionResponse> {
    let (ver, ts) = read_version();
    Json(VersionResponse {
        version: ver,
        updated: ts.parse().unwrap_or(0),
        service: "AKA-00".into(),
    })
}

async fn check_update(State(_s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    let (cur_ver, cur_ts_str) = read_version();
    let cur_ts: i64 = cur_ts_str.parse().unwrap_or(0);

    let check_url = std::env::var("OTA_CHECK_URL").unwrap_or_default();

    let (remote_ts, latest_ver, hw_desc, sw_desc, url) = if check_url.is_empty() {
        (0, String::new(), String::new(), String::new(), String::new())
    } else {
        fetch_remote(&check_url).await.unwrap_or_default()
    };

    // 版本号相同 → 同步时间戳
    let remote_ver = latest_ver.trim_start_matches('v');
    let cur_ver_clean = cur_ver.trim_start_matches('v');
    if !remote_ver.is_empty() && cur_ver_clean == remote_ver {
        if remote_ts > cur_ts {
            let _ = std::fs::write(version_file(), format!("{}@{}", cur_ver, remote_ts));
        }
        return Json(serde_json::json!({
            "current_version": cur_ver,
            "current_updated": cur_ts,
            "remote_updated": remote_ts,
            "update_available": false,
            "latest_version": latest_ver,
            "hardware_desc": hw_desc,
            "software_desc": sw_desc,
            "url": url,
        }));
    }

    let has_update = remote_ts > 0 && remote_ts > cur_ts;
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

async fn do_upgrade(State(_s): State<Arc<AppState>>) -> Json<serde_json::Value> {
    let check_url = std::env::var("OTA_CHECK_URL").unwrap_or_default();
    if check_url.is_empty() {
        return Json(serde_json::json!({"status": "error", "message": "未配置 OTA_CHECK_URL"}));
    }

    let (url,) = match fetch_remote(&check_url).await {
        Ok((_, _, _, _, url)) if !url.is_empty() => (url,),
        _ => return Json(serde_json::json!({"status": "error", "message": "未找到下载地址"})),
    };

    // 后台下载并安装
    tokio::spawn(async move {
        let _ = download_and_install(&url).await;
    });

    Json(serde_json::json!({"status": "ok", "message": "正在后台下载安装"}))
}

async fn download_and_install(url: &str) -> Result<(), String> {
    let client = reqwest::Client::new();
    let resp = client.get(url).send().await.map_err(|e| e.to_string())?;
    let bytes = resp.bytes().await.map_err(|e| e.to_string())?;

    let ota = ota_dir();
    std::fs::create_dir_all(&ota).map_err(|e| e.to_string())?;
    let tmp = ota.join("download.tmp");
    std::fs::write(&tmp, &bytes).map_err(|e| e.to_string())?;

    write_install_script(&tmp);
    Ok(())
}

fn write_install_script(firmware: &PathBuf) {
    let ota = ota_dir();
    let app = app_dir();
    let script = ota.join("install.sh");
    let fw = firmware.to_string_lossy();

    let content = format!(r#"#!/bin/sh
set -e
echo "[OTA] stopping..."
kill $(pgrep -f "dora") 2>/dev/null || true
sleep 2

echo "[OTA] extracting..."
rm -rf {ota}/staging
mkdir -p {ota}/staging

MAGIC=$(head -c2 "{fw}" 2>/dev/null)
if [ "$MAGIC" = "$(printf '\x1f\x8b')" ]; then
    tar xzf "{fw}" -C {ota}/staging
else
    sed -n '/^#__PAYLOAD_BELOW__$/,$p' "{fw}" | tail -n +2 | python3 -c "
import base64, sys, tarfile, tempfile, os
tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.tar.gz')
tmp.write(base64.b64decode(sys.stdin.buffer.read()))
tmp.close()
tarfile.open(tmp.name, mode='r:gz').extractall(path='{ota}/staging')
os.unlink(tmp.name)
"
fi

# 进入子目录（如有）
for d in {ota}/staging/*/; do
    [ -f "$d/run.py" ] && {{ cd "$d"; break; }}
done
cd {ota}/staging 2>/dev/null || true

echo "[OTA] installing..."
for item in * .[!.]*; do
    [ "$item" = "." ] && continue
    [ "$item" = ".." ] && continue
    [ ! -e "$item" ] && continue
    dst="{app}/$item"
    [ -d "$dst" ] && rm -rf "$dst"
    cp -r "$item" "$dst"
done

chmod +x {app}/*.sh 2>/dev/null || true
rm -rf {ota}/staging
rm -f "{fw}"

echo "[OTA] restarting..."
cd {app} && exec ./init.sh
"#, ota = ota.to_string_lossy(), app = app.to_string_lossy(), fw = fw);

    let _ = std::fs::write(&script, &content);
    #[cfg(unix)]
    { use std::os::unix::fs::PermissionsExt; let _ = std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o755)); }
    let _ = std::process::Command::new("/bin/sh")
        .arg(&script)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn();
}

async fn fetch_remote(url: &str) -> Result<(i64, String, String, String, String), String> {
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(5))
        .build()
        .map_err(|e| e.to_string())?;

    let resp = client.get(url).send().await.map_err(|e| e.to_string())?;
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
