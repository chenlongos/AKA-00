//! 摄像头服务层 —— 管理帧广播、设备开关、JPEG 编码
//!
//! 对应 Python 项目 `app/services/camera_service.py` 的职责。

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use dora_node_api::DoraNode;
use eyre::Context;
use tokio::sync::{watch, Mutex};

const SRC_WIDTH: u32 = 640;
const SRC_HEIGHT: u32 = 480;
const JPEG_QUALITY: u8 = 70;

/// 摄像头服务：单例，被 routes 和 dora 事件循环共享
pub struct CameraService {
    /// MJPEG 帧广播
    frame_tx: watch::Sender<Vec<u8>>,
    /// 摄像头开关状态（true = 前端请求开启 + 已发送 dora start）
    active: Arc<AtomicBool>,
    /// dora 节点句柄，用于发送 control 消息到 camera 节点
    node: Arc<Mutex<DoraNode>>,
}

impl CameraService {
    pub fn new(node: Arc<Mutex<DoraNode>>) -> Self {
        let (frame_tx, _rx) = watch::channel(Vec::new());
        std::mem::forget(_rx);

        Self {
            frame_tx,
            active: Arc::new(AtomicBool::new(false)),
            node,
        }
    }

    // ── 查询 ──

    /// 摄像头是否开启（供 route 使用）
    pub fn is_active(&self) -> bool {
        self.active.load(Ordering::Relaxed)
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

        let mut node = self.node.lock().await;
        node.send_output_bytes("control".into(), BTreeMap::new(), 5, b"start")
            .wrap_err("send dora start")?;

        println!("[camera-service] 📷 on");
        Ok(())
    }

    /// 关闭摄像头：发送 dora "stop" → camera 节点释放设备
    pub async fn close(&self) -> eyre::Result<()> {
        if !self.active.swap(false, Ordering::Relaxed) {
            return Ok(()); // 已关闭
        }

        let mut node = self.node.lock().await;
        node.send_output_bytes("control".into(), BTreeMap::new(), 4, b"stop")
            .wrap_err("send dora stop")?;

        println!("[camera-service] ⏸  off");
        Ok(())
    }

    // ── 帧处理 ──

    /// 接收 dora 原始 RGB 帧，编码 JPEG 并广播到所有 /stream 订阅者
    pub fn push_frame(&self, data: &dora_node_api::ArrowData) {
        if !self.active.load(Ordering::Relaxed) {
            return;
        }

        if let Some(jpeg) = Self::rgb_to_jpeg(data) {
            self.frame_tx.send_replace(jpeg);
        }
    }
}

impl CameraService {
    /// Arrow UInt8 RGB → JPEG bytes
    fn rgb_to_jpeg(data: &dora_node_api::ArrowData) -> Option<Vec<u8>> {
        use arrow::array::AsArray;
        let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
        let rgb = uint8_arr.values();

        let expected = (SRC_WIDTH * SRC_HEIGHT * 3) as usize;
        if rgb.len() < expected {
            return None;
        }

        let mut buf = Vec::new();
        let mut enc =
            image::codecs::jpeg::JpegEncoder::new_with_quality(&mut buf, JPEG_QUALITY);
        enc.encode(
            &rgb[..expected],
            SRC_WIDTH,
            SRC_HEIGHT,
            image::ExtendedColorType::Rgb8,
        )
        .ok()?;
        Some(buf)
    }
}
