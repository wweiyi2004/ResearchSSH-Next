//! The `SshProvider` abstraction.
//!
//! Everything that actually talks SSH lives behind this trait so the rest of the
//! core (sessions, FFI, scheduling) is independent of the transport. The Mock
//! implementation is always available; the real [`russh`](crate::providers::russh)
//! backend is compiled behind the `russh` cargo feature.

use crate::secret::Secret;
use crate::session::HostKeyGate;
use crate::{CoreError, CoreResult};
use async_trait::async_trait;
use tokio::sync::mpsc;

/// Which provider backs a session. Numeric values are part of the C ABI.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RsProviderKind {
    /// In-process simulator used for framework/UI development and tests.
    Mock = 0,
    /// Real SSH via the `russh` crate; requires the `russh` feature.
    Russh = 1,
}

/// Connection parameters handed to a provider. Secrets are intentionally kept out
/// of this struct: they travel through [`SshProvider::set_secret`] so they can be
/// zeroized independently and never appear in logs or the QML layer.
#[derive(Debug, Clone)]
pub struct ConnectionConfig {
    /// Hostname or IP (UTF-8).
    pub host: String,
    /// TCP port.
    pub port: u16,
    /// Login user name (UTF-8).
    pub username: String,
    /// Which provider to use.
    pub kind: RsProviderKind,
}

/// Events a provider pushes asynchronously toward its session.
#[derive(Debug)]
pub enum ProviderEvent {
    /// Bytes received from the remote (terminal output).
    Data(Vec<u8>),
    /// The remote/connection closed; carries a human-readable reason.
    Disconnected(String),
    /// A transport-level error occurred.
    Error(CoreError),
}

/// The sink a provider uses to emit [`ProviderEvent`]s. Cloneable so a provider
/// can hand it to background tasks.
#[derive(Clone)]
pub struct ProviderSink {
    tx: mpsc::UnboundedSender<ProviderEvent>,
}

impl ProviderSink {
    /// Wrap a channel sender.
    pub fn new(tx: mpsc::UnboundedSender<ProviderEvent>) -> Self {
        Self { tx }
    }

    /// Emit an event. Errors (receiver gone) are swallowed: the session is tearing
    /// down and the provider has nothing useful to do about it.
    pub fn emit(&self, ev: ProviderEvent) {
        let _ = self.tx.send(ev);
    }
}

/// Transport abstraction. Implementors must be `Send` so the session driver task
/// can own them across `.await` points on the multi-threaded runtime.
///
/// Implementations must never panic across an `.await` that the FFI layer relies
/// on; all failure is reported via [`CoreError`] / [`ProviderEvent::Error`].
#[async_trait]
pub trait SshProvider: Send {
    /// Store a sensitive credential (e.g. password / key bytes). The default
    /// implementation drops it (and thus zeroizes it).
    fn set_secret(&mut self, _secret: Secret) {}

    /// Provide the host-key confirmation gate. Default: ignore (providers that do
    /// not verify host keys, e.g. the mock). Real providers use it to ask the UI.
    fn set_host_key_gate(&mut self, _gate: HostKeyGate) {}

    /// Hand over a file (SFTP) provider established during [`connect`](Self::connect),
    /// if this backend supports file transfer. Called once after a successful
    /// connect. Default: none.
    fn take_file_provider(&mut self) -> Option<Box<dyn crate::fs::FileProvider>> {
        None
    }

    /// Establish the connection. On success the provider may immediately start
    /// pushing [`ProviderEvent`]s through `sink` (e.g. a login banner).
    async fn connect(&mut self, sink: ProviderSink) -> CoreResult<()>;

    /// Send bytes to the remote (e.g. a command or keystrokes).
    async fn send(&mut self, data: &[u8]) -> CoreResult<()>;

    /// Disconnect cleanly. Should be idempotent.
    async fn disconnect(&mut self) -> CoreResult<()>;
}

/// Build a provider for the requested kind.
///
/// Returns [`RsErrorCode::ProviderUnavailable`] if the kind is not compiled into
/// this build (e.g. `Russh` without the `russh` feature).
pub fn create_provider(config: &ConnectionConfig) -> CoreResult<Box<dyn SshProvider>> {
    match config.kind {
        RsProviderKind::Mock => Ok(Box::new(crate::providers::mock::MockProvider::new(
            config.clone(),
        ))),
        RsProviderKind::Russh => {
            #[cfg(feature = "russh")]
            {
                Ok(Box::new(crate::providers::russh::RusshProvider::new(
                    config.clone(),
                )))
            }
            #[cfg(not(feature = "russh"))]
            {
                Err(CoreError::with_detail(
                    crate::RsErrorCode::ProviderUnavailable,
                    "russh provider not compiled in; build with --features russh",
                ))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mock_provider_is_always_available() {
        let cfg = ConnectionConfig {
            host: "example.com".into(),
            port: 22,
            username: "researcher".into(),
            kind: RsProviderKind::Mock,
        };
        assert!(create_provider(&cfg).is_ok());
    }

    #[cfg(not(feature = "russh"))]
    #[test]
    fn russh_provider_unavailable_without_feature() {
        let cfg = ConnectionConfig {
            host: "example.com".into(),
            port: 22,
            username: "researcher".into(),
            kind: RsProviderKind::Russh,
        };
        let err = create_provider(&cfg)
            .err()
            .expect("expected unavailable error");
        assert_eq!(err.code, crate::RsErrorCode::ProviderUnavailable);
    }
}
