//! Re-export dora-node-api-c symbols into a linkable staticlib.
//!
//! 同时提供一份 `catch_unwind` 包裹的"安全"变体 (`safe_*`)，解决
//! `dora-node-api-c:189-192` 的上游 `todo!()` panic 会跨 FFI 直接 abort 调用进程的问题。
//! 当前的 C++ 节点（camera-node）继续用 unsafe re-exports，迁移要单独做；本文件留出
//! safe_* 系列供后续接 C 接口或换上游 PR。

// 原始 re-exports —— C++ 当前链路，保留 ABI 不变
pub use dora_node_api_c::*;

use std::panic;
use std::ptr;

/// 把上游 `todo!()`（非 UInt8 / Non-Null arrow）转成 typed error。
///
/// C 端约定：`*out_ptr` 设 nullptr、`*out_len` 设 0、`return` 负数（标准 errno 风格）。
///
/// 用法（一旦 camera-node 决定切过来）：
/// ```ignore
/// char* data = nullptr; size_t len = 0;
/// if (safe_read_dora_input_data(event, &data, &len) < 0) {
///     // 上游抛 panic 已被吞，本次 read 没拿到数据
///     free_dora_event(event);
///     continue;
/// }
/// ```
///
/// 注意：`catch_unwind` 只 catch unwinding panic（C++ 那边 unwind 不进 Rust），
/// `todo!()` 走 unwinding path，因此这套能拦住。但 `abort()` 类 panic 拦不住——
/// 上游这次是 `todo!()`，是 unwind，符合拦截目标。如果上游改用
/// `std::process::exit` 或直接 abort，这层就没办法了，需要上游本身改。
pub unsafe extern "C" fn safe_read_dora_input_data(
    event: *const (),
    out_ptr: *mut *const u8,
    out_len: *mut usize,
) -> i32 {
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| unsafe {
        dora_node_api_c::read_dora_input_data(event, out_ptr, out_len)
    }));
    match result {
        Ok(_) => 0,
        Err(_) => {
            if !out_ptr.is_null() {
                *out_ptr = ptr::null();
            }
            if !out_len.is_null() {
                *out_len = 0;
            }
            -1
        }
    }
}

pub unsafe extern "C" fn safe_read_dora_input_id(
    event: *const (),
    out_ptr: *mut *const u8,
    out_len: *mut usize,
) -> i32 {
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| unsafe {
        dora_node_api_c::read_dora_input_id(event, out_ptr, out_len)
    }));
    match result {
        Ok(_) => 0,
        Err(_) => {
            if !out_ptr.is_null() {
                *out_ptr = ptr::null();
            }
            if !out_len.is_null() {
                *out_len = 0;
            }
            -1
        }
    }
}
