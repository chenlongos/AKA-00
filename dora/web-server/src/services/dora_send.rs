//! 统一 dora-send helper，给 services/{demo,camera,motor,arm}.rs 用
//!
//! 包装 `Arc<Mutex<DoraNode>>::send_output_bytes` + 2s 超时。
//!
//! 为什么不直接 `node.lock().await + send_output_bytes(...)`：
//! - send_output_bytes 是同步阻塞调用。dora-daemon 死了之后 Unix socket 写会无限阻塞。
//! - 阻塞期间持有 `tokio::sync::Mutex` 守卫，axum worker 无法服务其它请求；
//!   joystick 10Hz 流量耗光 worker 时整个 web-server 静默死亡。
//! - `tokio::time::timeout` 取消 future 时，async block 被 drop → Mutex guard 析构释放锁。
//! - 底层阻塞 send 仍在线程栈上跑直到完成（已知限制：会泄漏 1 个被阻塞线程），
//!   但下一调用能立即 lock 到 mutex——所以收益主要是 HTTP handler 不再卡死。

use std::collections::BTreeMap;
use std::sync::Arc;
use std::time::Duration;

use dora_node_api::DoraNode;
use eyre::eyre;
use tokio::sync::Mutex;

/// 单次 send 的硬超时（秒）。超过则返回错误，daemon 大概率死了。
pub const TIMEOUT_SECS: u64 = 2;

/// 发送 bytes 到指定的 dora output id，超时返回错误。
pub async fn send_output(node: &Arc<Mutex<DoraNode>>, id: &str, bytes: &[u8]) -> eyre::Result<()> {
    // id_owned 留一份外层作用域，timeout 错误信息里要用；async block 内再 clone 一份。
    let id_owned = id.to_string();

    let fut = {
        let id = id_owned.clone();
        let bytes = bytes.to_vec();
        async move {
            let mut node = node.lock().await;
            node.send_output_bytes(id.clone().into(), BTreeMap::new(), bytes.len(), &bytes)
                .map_err(|e| eyre!("send '{id}': {e:?}"))?;
            Ok::<_, eyre::Report>(())
        }
    };

    match tokio::time::timeout(Duration::from_secs(TIMEOUT_SECS), fut).await {
        Ok(inner) => inner,
        Err(_) => {
            log::warn!(
                "[dora-send] '{id_owned}' timed out after {TIMEOUT_SECS}s (daemon likely hung)"
            );
            Err(eyre!(
                "dora send '{id_owned}' timed out after {TIMEOUT_SECS}s (daemon likely hung)"
            ))
        }
    }
}
