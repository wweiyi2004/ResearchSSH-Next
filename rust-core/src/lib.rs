//! ResearchSSH-Next security core.
//!
//! This crate owns everything security- and IO-sensitive: the async runtime,
//! SSH/SFTP sessions (behind the [`provider::SshProvider`] abstraction), session
//! state machines, task scheduling and (in the future) VT/ANSI parsing. It is
//! consumed by the C++/Qt layer exclusively through the stable C ABI defined in
//! [`ffi`].
//!
//! # Layering / safety rules enforced here
//! * `#![deny(unsafe_code)]` at the crate root means *no* module may use `unsafe`.
//! * The single exception is the [`ffi`] module, which is annotated with
//!   `#[allow(unsafe_code)]`. That is the only place raw pointers cross the
//!   boundary, and every `unsafe` block there documents its safety contract.
//! * No Rust or C++ object, exception or panic is ever allowed to cross the FFI
//!   boundary; see [`ffi`] for the `catch_unwind` strategy.

#![deny(unsafe_code)]
#![warn(missing_docs)]
#![warn(rust_2018_idioms)]

pub mod event;
pub mod fs;
pub mod provider;
pub mod providers;
pub mod runtime;
pub mod secret;
pub mod session;
pub mod task;

// The FFI module is the ONLY place allowed to use `unsafe`.
#[allow(unsafe_code)]
pub mod ffi;

use std::fmt;

/// Stable error codes returned across the C ABI.
///
/// `0` always means success. The numeric values are part of the ABI contract and
/// must never be reordered or reused. A human-readable, UTF-8 description for any
/// code is available via [`ffi::rscore_error_message`].
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RsErrorCode {
    /// Operation succeeded.
    Ok = 0,
    /// A required pointer argument was null.
    NullArgument = 1,
    /// A C string argument was not valid UTF-8.
    InvalidUtf8 = 2,
    /// The operation is not valid in the current session state.
    InvalidState = 3,
    /// The requested provider or session capability is not available.
    ProviderUnavailable = 4,
    /// Establishing the connection failed.
    ConnectFailed = 5,
    /// The session is not connected.
    NotConnected = 6,
    /// The operation was cancelled.
    Cancelled = 7,
    /// The async runtime reported a failure (e.g. the worker is gone).
    RuntimeError = 8,
    /// A panic was caught at the FFI boundary (should never reach the caller as UB).
    Panic = 9,
    /// An unexpected internal error occurred.
    Internal = 10,
}

impl RsErrorCode {
    /// Static, UTF-8 description of the error code (no allocation, never freed).
    pub fn message(self) -> &'static str {
        match self {
            RsErrorCode::Ok => "ok",
            RsErrorCode::NullArgument => "a required pointer argument was null",
            RsErrorCode::InvalidUtf8 => "argument was not valid UTF-8",
            RsErrorCode::InvalidState => "operation invalid in current session state",
            RsErrorCode::ProviderUnavailable => "requested provider or capability is unavailable",
            RsErrorCode::ConnectFailed => "failed to establish the connection",
            RsErrorCode::NotConnected => "session is not connected",
            RsErrorCode::Cancelled => "operation was cancelled",
            RsErrorCode::RuntimeError => "async runtime error",
            RsErrorCode::Panic => "a panic was caught at the FFI boundary",
            RsErrorCode::Internal => "internal error",
        }
    }
}

/// Internal error type used throughout the core. Carries a code plus an optional
/// detail message; it is converted to [`RsErrorCode`] (and surfaced as an error
/// event with a message) at the FFI boundary.
#[derive(Debug, Clone)]
pub struct CoreError {
    /// Stable error code.
    pub code: RsErrorCode,
    /// Optional human-readable detail (UTF-8).
    pub detail: Option<String>,
}

impl CoreError {
    /// Create an error with just a code.
    pub fn new(code: RsErrorCode) -> Self {
        Self { code, detail: None }
    }

    /// Create an error with a code and a detail message.
    pub fn with_detail(code: RsErrorCode, detail: impl Into<String>) -> Self {
        Self {
            code,
            detail: Some(detail.into()),
        }
    }

    /// The message to surface: the detail if present, else the static code message.
    pub fn message(&self) -> String {
        self.detail
            .clone()
            .unwrap_or_else(|| self.code.message().to_string())
    }
}

impl fmt::Display for CoreError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.detail {
            Some(d) => write!(f, "{} ({:?})", d, self.code),
            None => write!(f, "{}", self.code.message()),
        }
    }
}

impl std::error::Error for CoreError {}

/// Convenience result alias used internally.
pub type CoreResult<T> = Result<T, CoreError>;

/// Semantic version string of the core, surfaced over FFI via
/// [`ffi::rscore_version`].
pub const CORE_VERSION: &str = env!("CARGO_PKG_VERSION");

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn error_codes_have_messages() {
        assert_eq!(RsErrorCode::Ok.message(), "ok");
        assert!(!RsErrorCode::ConnectFailed.message().is_empty());
    }

    #[test]
    fn core_error_prefers_detail() {
        let e = CoreError::with_detail(RsErrorCode::ConnectFailed, "host unreachable");
        assert_eq!(e.message(), "host unreachable");
        let e2 = CoreError::new(RsErrorCode::NotConnected);
        assert_eq!(e2.message(), RsErrorCode::NotConnected.message());
    }
}
