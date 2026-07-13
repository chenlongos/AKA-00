//! 摄像头服务层 —— 管理帧广播、设备开关、JPEG 编码
//!
//! 对应 Python 项目 `app/services/camera_service.py` 的职责。

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use dora_node_api::DoraNode;
use eyre::Context;
use tokio::sync::{watch, Mutex};

use super::dora_send;

/// 摄像头服务：单例，被 routes 和 dora 事件循环共享
pub struct CameraService {
    /// MJPEG 帧广播
    frame_tx: watch::Sender<Vec<u8>>,
    /// 摄像头开关状态（true = 前端请求开启 + 已发送 dora start）
    active: Arc<AtomicBool>,
    /// dora 节点句柄，用于发送 control 消息到 camera 节点
    node: Arc<Mutex<DoraNode>>,
    /// 配置（分辨率、JPEG 质量）
    config: dora_config::CameraConfig,
}

impl CameraService {
    pub fn new(node: Arc<Mutex<DoraNode>>, config: dora_config::CameraConfig) -> Self {
        let (frame_tx, _rx) = watch::channel(Vec::new());
        std::mem::forget(_rx);

        Self {
            frame_tx,
            active: Arc::new(AtomicBool::new(false)),
            node,
            config,
        }
    }

    // ── 查询 ──

    /// 摄像头是否开启（供 route 使用）
    pub fn is_active(&self) -> bool {
        self.active.load(Ordering::Relaxed)
    }
    /// 从 state-node 同步摄像头状态
    pub fn set_active(&self, on: bool) {
        self.active.store(on, Ordering::Relaxed);
    }

    /// 订阅帧流（供 /stream 使用）
    pub fn subscribe(&self) -> watch::Receiver<Vec<u8>> {
        self.frame_tx.subscribe()
    }

    /// 获取当前帧（供 /snapshot 使用）
    pub fn current_frame(&self) -> Vec<u8> {
        self.frame_tx.borrow().clone()
    }

    // ── 控制 ──

    /// 开启摄像头：发送 dora "start" → camera 节点打开设备并开始抓帧
    pub async fn open(&self) -> eyre::Result<()> {
        if self.active.swap(true, Ordering::Relaxed) {
            return Ok(()); // 已开启
        }

        dora_send::send_output(&self.node, "control", b"start")
            .await
            .wrap_err("send dora start")?;

        log::info!("[camera-service] 📷 on");
        Ok(())
    }

    /// 关闭摄像头：发送 dora "stop" → camera 节点释放设备
    pub async fn close(&self) -> eyre::Result<()> {
        if !self.active.swap(false, Ordering::Relaxed) {
            return Ok(()); // 已关闭
        }

        dora_send::send_output(&self.node, "control", b"stop")
            .await
            .wrap_err("send dora stop")?;

        log::info!("[camera-service] ⏸  off");
        Ok(())
    }

    // ── 帧处理 ──

    /// 接收 dora 帧数据，广播到所有 /stream 订阅者
    /// 数据可能是 JPEG（camera-node 直接发）或原始 RGB（旧版兼容）
    pub fn push_frame(&self, data: &dora_node_api::ArrowData) {
        use arrow::array::AsArray;
        use std::sync::atomic::Ordering;

        let active = self.active.load(Ordering::Relaxed);
        if !active {
            return;  // 摄像头关闭状态，静默丢弃
        }

        let raw = data.as_primitive::<arrow::datatypes::UInt8Type>().values();

        if raw.is_empty() {
            log::warn!("[camera-svc] push_frame: empty data (len=0), dropping");
            return;
        }

        // JPEG 头部检测
        let is_jpeg = raw.len() >= 2 && raw[0] == 0xFF && raw[1] == 0xD8;
        if is_jpeg {
            self.frame_tx.send_replace(raw.to_vec());
            return;
        }

        log::warn!(
            "[camera-svc] push_frame: not JPEG, header=[{:02X} {:02X}], len={}",
            raw.first().unwrap_or(&0),
            raw.get(1).unwrap_or(&0),
            raw.len()
        );

        // 旧版 RGB → JPEG
        if let Some(jpeg) = Self::encode_rgb_to_jpeg(raw, self.config.width, self.config.height, self.config.jpeg_quality) {
            self.frame_tx.send_replace(jpeg);
        } else {
            log::warn!("[camera-svc] push_frame: not JPEG, RGB encode failed (len={})", raw.len());
        }
    }
}

impl CameraService {
    fn encode_rgb_to_jpeg(rgb: &[u8], w: u32, h: u32, quality: u8) -> Option<Vec<u8>> {
        let expected = (w * h * 3) as usize;
        if rgb.len() < expected {
            return None;
        }
        let mut buf = Vec::new();
        let mut enc =
            image::codecs::jpeg::JpegEncoder::new_with_quality(&mut buf, quality);
        enc.encode(&rgb[..expected], w, h, image::ExtendedColorType::Rgb8)
            .ok()?;
        Some(buf)
    }
}
