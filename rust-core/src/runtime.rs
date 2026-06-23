//! The async runtime that powers the core.
//!
//! A single multi-threaded Tokio runtime is created per [`CoreRuntime`] (one per
//! FFI `RsCore`). All sessions spawn their driver tasks onto it. The runtime is
//! owned by the core and torn down when the core is destroyed.

use crate::{CoreError, CoreResult, RsErrorCode};
use std::sync::Arc;
use tokio::runtime::{Builder, Handle, Runtime};

/// Owns the Tokio runtime and hands out cheap [`Handle`]s to sessions.
pub struct CoreRuntime {
    runtime: Runtime,
}

impl CoreRuntime {
    /// Build a multi-threaded runtime with timers enabled.
    pub fn new() -> CoreResult<Arc<Self>> {
        let runtime = Builder::new_multi_thread()
            .enable_all() // time + IO; the real SSH provider needs networking
            .thread_name("researchssh-core")
            .build()
            .map_err(|e| {
                CoreError::with_detail(RsErrorCode::RuntimeError, format!("runtime build: {e}"))
            })?;
        Ok(Arc::new(Self { runtime }))
    }

    /// A clonable handle for spawning tasks and blocking from non-runtime threads.
    pub fn handle(&self) -> Handle {
        self.runtime.handle().clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn runtime_builds_and_runs() {
        let rt = CoreRuntime::new().expect("runtime");
        let v = rt.handle().block_on(async { 1 + 1 });
        assert_eq!(v, 2);
    }
}
