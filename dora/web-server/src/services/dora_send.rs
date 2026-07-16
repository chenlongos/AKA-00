//! 统一 dora-send helper，给 services/{demo,camera,motor,arm}.rs 用
//!
//! 包装 `Arc<Mutex<DoraNode>>::send_output_bytes` + 2s 超时。
//!
//! 设计取舍：
//! - send_output_bytes 是同步调用。daemon 活着时 <1ms，直接在当前 task 执行
//!   最快（spawn_blocking 在单核 RISC-V 上线程池调度开销 5-20ms，摇杆 10Hz
//!   流量下延迟不可接受）。
//! - timeout 只在 daemon 挂掉时触发：取消 future → drop async block →
//!   MutexGuard 析构释放锁。底层 send 的线程泄漏是已知限制（daemon 挂了本
//!   来就需要重启整个系统，1 个泄漏线程不是问题）。

use std::collections::BTreeMap;
use std::sync::Arc;
use std::time::Duration;

use dora_node_api::DoraNode;
use eyre::eyre;
use tokio::sync::Mutex;

/// 单次 send 的硬超时（秒）。
pub const TIMEOUT_SECS: u64 = 2;

/// 发送 bytes 到指定的 dora output id，超时返回错误。
pub async fn send_output(node: &Arc<Mutex<DoraNode>>, id: &str, bytes: &[u8]) -> eyre::Result<()> {
    let id_owned = id.to_string();
    let bytes_owned = bytes.to_vec();
    let node = node.clone();

    let fut = async move {
        let mut node = node.lock().await;
        node.send_output_bytes(
            id_owned.clone().into(),
            BTreeMap::new(),
            bytes_owned.len(),
            &bytes_owned,
        )
        .map_err(|e| eyre!("send '{id_owned}': {e:?}"))
    };

    match tokio::time::timeout(Duration::from_secs(TIMEOUT_SECS), fut).await {
        Ok(inner) => inner,
        Err(_) => {
            log::warn!(
                "[dora-send] '{id}' timed out after {TIMEOUT_SECS}s (daemon likely hung)",
            );
            Err(eyre!(
                "dora send '{id}' timed out after {TIMEOUT_SECS}s (daemon likely hung)"
            ))
        }
    }
}
