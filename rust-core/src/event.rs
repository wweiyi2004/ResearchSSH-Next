//! Session events and their C-ABI representation.
//!
//! Internally the core works with the rich [`SessionEvent`] enum. At the FFI
//! boundary these are flattened into [`RsEvent`] (a `#[repr(C)]` POD struct) and
//! delivered to C++ in **batches** — never one byte / one event at a time — to
//! keep the cost of crossing the boundary amortised (see the requirement that
//! terminal data must be transferred in bulk).

use crate::RsErrorCode;

/// Lifecycle state of a session. The numeric values are part of the ABI.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RsSessionState {
    /// Created but no connection attempt has started.
    Idle = 0,
    /// A connection attempt is in progress.
    Connecting = 1,
    /// Connected and ready to exchange data.
    Connected = 2,
    /// A disconnect was requested and is in progress.
    Disconnecting = 3,
    /// Cleanly disconnected.
    Disconnected = 4,
    /// Terminated due to an error.
    Failed = 5,
}

/// Discriminator for [`RsEvent`].
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RsEventKind {
    /// `state` is meaningful; the session changed lifecycle state.
    StateChanged = 0,
    /// `data`/`data_len` carry terminal bytes (already batched).
    Data = 1,
    /// `error_code`/`message` describe an error.
    Error = 2,
    /// The server presented an unrecognised host key; `message` carries its
    /// fingerprint. The UI must confirm via `rscore_session_confirm_host_key`.
    HostKeyPrompt = 3,
}

/// C-ABI view of a single event.
///
/// # Pointer ownership (ABI contract)
/// `data` and `message` point into buffers owned by the Rust core. They are valid
/// **only for the duration of the [`RsEventCallback`](crate::ffi::RsEventCallback)
/// invocation** that delivered them. The C++ side MUST copy anything it needs
/// before returning from the callback. Rust frees the backing buffers immediately
/// after the callback returns.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct RsEvent {
    /// Which union-ish fields are valid.
    pub kind: RsEventKind,
    /// Valid when `kind == StateChanged`.
    pub state: RsSessionState,
    /// Valid when `kind == Error`.
    pub error_code: RsErrorCode,
    /// Terminal bytes; valid when `kind == Data`. May be null otherwise.
    pub data: *const u8,
    /// Length of `data` in bytes.
    pub data_len: usize,
    /// UTF-8, null-terminated message; valid when `kind == Error`. May be null.
    pub message: *const std::os::raw::c_char,
}

/// Rich, owned event used inside the core before it is flattened to [`RsEvent`].
#[derive(Debug, Clone)]
pub enum SessionEvent {
    /// Lifecycle transition.
    StateChanged(RsSessionState),
    /// A chunk of terminal output bytes.
    Data(Vec<u8>),
    /// An error with a code and human-readable message.
    Error {
        /// Stable error code.
        code: RsErrorCode,
        /// UTF-8 detail message.
        message: String,
    },
    /// The server's host key needs user confirmation; carries its fingerprint.
    HostKeyPrompt {
        /// Host-key fingerprint (e.g. `SHA256:…`).
        fingerprint: String,
    },
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn event_kinds_are_distinct() {
        assert_ne!(RsEventKind::Data as i32, RsEventKind::Error as i32);
        assert_eq!(RsSessionState::Connected as i32, 2);
    }
}
