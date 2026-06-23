//! Session lifecycle: a small state machine driven by an async task.
//!
//! A [`Session`] owns a command channel to a background *driver* task spawned on
//! the core's Tokio runtime. The driver talks to an [`SshProvider`] and forwards
//! everything that happens (state transitions, terminal data, errors) to an
//! [`EventEmitter`]. The FFI layer plugs in an emitter that batches events and
//! invokes the registered C callback exactly once per batch.
//!
//! Cancellation is delivered out-of-band through a shared [`Notify`] so it can
//! interrupt an in-flight `connect`/`send` even while the driver is awaiting it.

use crate::event::{RsSessionState, SessionEvent};
use crate::fs::{FilePayload, FileProvider, FileResult};
use crate::provider::{
    create_provider, ConnectionConfig, ProviderEvent, ProviderSink, SshProvider,
};
use crate::secret::Secret;
use crate::{CoreError, CoreResult, RsErrorCode};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::runtime::Handle;
use tokio::sync::{mpsc, oneshot, Notify};
use tokio::task::JoinHandle;

/// Sink for delivering batched session events to the outside world (FFI / tests).
pub trait EventEmitter: Send + Sync {
    /// Deliver a batch of events. Implementations must treat this as one atomic
    /// hand-off (the FFI emitter performs exactly one callback per call).
    fn emit_batch(&self, events: Vec<SessionEvent>);

    /// Permanently stop forwarding events to the underlying sink. After this
    /// returns, no in-flight or future `emit_batch` will reach the C callback.
    /// Called at teardown so that a driver task which outlived its join timeout
    /// (and was therefore detached) can never touch freed `user_data`. Default:
    /// no-op (test emitters don't need it).
    fn deactivate(&self) {}
}

/// Shared slot holding the sender for a pending host-key decision.
pub type HostKeyPending = Arc<Mutex<Option<oneshot::Sender<bool>>>>;

/// Lets a provider ask the UI to confirm an unrecognised host key *mid-connect*.
///
/// The provider emits a [`SessionEvent::HostKeyPrompt`] directly through the
/// shared emitter — so it reaches the UI even though the driver is parked on
/// `connect()` and not draining its provider-event channel — and then awaits the
/// user's decision, which arrives via [`Session::confirm_host_key`].
#[derive(Clone)]
pub struct HostKeyGate {
    emitter: Arc<dyn EventEmitter>,
    pending: HostKeyPending,
}

impl HostKeyGate {
    /// Emit the prompt with `fingerprint` and await accept (`true`) / reject.
    /// Returns `false` if the decision channel is dropped (e.g. session torn down).
    pub async fn confirm(&self, fingerprint: String) -> bool {
        let (tx, rx) = oneshot::channel();
        *self.pending.lock().expect("hostkey lock") = Some(tx);
        self.emitter
            .emit_batch(vec![SessionEvent::HostKeyPrompt { fingerprint }]);
        rx.await.unwrap_or(false)
    }
}

/// Sink for delivering file-operation results to the FFI file callback. Parallel
/// to [`EventEmitter`] but for the SFTP/file pipeline.
pub trait FileEmitter: Send + Sync {
    /// Deliver a batch of file results (one callback per call).
    fn emit_file_results(&self, results: Vec<FileResult>);

    /// See [`EventEmitter::deactivate`]: makes the emitter permanently inert so a
    /// detached driver task cannot invoke the C callback after teardown.
    fn deactivate(&self) {}
}

/// A file operation requested over the C ABI; carried with its request id.
enum FsOp {
    List(String),
    Stat(String),
    Read(String, u64),
    Write(String, Vec<u8>),
    Rename(String, String),
    Remove(String, bool),
    Mkdir(String),
    Copy(String, String),
}

/// Commands sent from the [`Session`] handle to its driver task.
enum Command {
    Connect,
    Send(Vec<u8>),
    Disconnect,
    SetSecret(Secret),
    SetPrivateKey {
        key_path: Option<String>,
        passphrase: Option<Secret>,
    },
    Fs {
        request_id: u64,
        op: FsOp,
    },
    Shutdown,
}

/// Shared slot for the optional file-result emitter (set after the FFI registers a
/// file callback).
type FileEmitterSlot = Arc<Mutex<Option<Arc<dyn FileEmitter>>>>;

/// Handle to a running session. Cloning is intentionally not provided; the FFI
/// layer owns exactly one boxed `Session` per `RsSession*`.
pub struct Session {
    cmd_tx: mpsc::UnboundedSender<Command>,
    cancel: Arc<Notify>,
    handle: Handle,
    join: Mutex<Option<JoinHandle<()>>>,
    host_key_pending: HostKeyPending,
    next_request_id: AtomicU64,
    file_emitter: FileEmitterSlot,
    /// Kept so teardown can deactivate the event sink even if the driver task is
    /// still running (see [`Session::shutdown_and_join`]).
    emitter: Arc<dyn EventEmitter>,
}

impl Session {
    /// Spawn the driver task and return a handle.
    pub fn spawn(handle: Handle, config: ConnectionConfig, emitter: Arc<dyn EventEmitter>) -> Self {
        let (cmd_tx, cmd_rx) = mpsc::unbounded_channel();
        let cancel = Arc::new(Notify::new());
        let cancel_driver = cancel.clone();
        let host_key_pending: HostKeyPending = Arc::new(Mutex::new(None));
        let gate = HostKeyGate {
            emitter: emitter.clone(),
            pending: host_key_pending.clone(),
        };
        let file_emitter: FileEmitterSlot = Arc::new(Mutex::new(None));
        let file_emitter_driver = file_emitter.clone();
        // Retained on the handle so teardown can deactivate it; the driver task gets
        // its own clone moved in below.
        let emitter_for_session = emitter.clone();
        let join = handle.spawn(async move {
            drive(
                config,
                emitter,
                cmd_rx,
                cancel_driver,
                gate,
                file_emitter_driver,
            )
            .await;
        });
        Self {
            cmd_tx,
            cancel,
            handle,
            join: Mutex::new(Some(join)),
            host_key_pending,
            next_request_id: AtomicU64::new(0),
            file_emitter,
            emitter: emitter_for_session,
        }
    }

    /// Request a connection.
    pub fn connect(&self) -> CoreResult<()> {
        self.send_cmd(Command::Connect)
    }

    /// Request a disconnect.
    pub fn disconnect(&self) -> CoreResult<()> {
        self.send_cmd(Command::Disconnect)
    }

    /// Send bytes (a command line / keystrokes) to the remote.
    pub fn send(&self, data: Vec<u8>) -> CoreResult<()> {
        self.send_cmd(Command::Send(data))
    }

    /// Hand a secret to the underlying provider (reserved for real providers).
    pub fn set_secret(&self, secret: Secret) -> CoreResult<()> {
        self.send_cmd(Command::SetSecret(secret))
    }

    /// Hand a private-key candidate (path + optional passphrase) to the provider.
    pub fn set_private_key(
        &self,
        key_path: Option<String>,
        passphrase: Option<Secret>,
    ) -> CoreResult<()> {
        self.send_cmd(Command::SetPrivateKey {
            key_path,
            passphrase,
        })
    }

    /// Cancel the in-flight operation (if any). Out-of-band: works even while the
    /// driver is awaiting `connect`/`send`.
    pub fn cancel(&self) {
        self.cancel.notify_waiters();
    }

    /// Deliver the user's host-key decision to a provider that is awaiting
    /// confirmation. No-op if nothing is pending.
    pub fn confirm_host_key(&self, accept: bool) {
        if let Some(tx) = self.host_key_pending.lock().expect("hostkey lock").take() {
            let _ = tx.send(accept);
        }
    }

    /// Register the file-result emitter (the FFI file callback). Results of all
    /// `fs_*` operations are delivered through it.
    pub fn set_file_emitter(&self, emitter: Arc<dyn FileEmitter>) {
        *self.file_emitter.lock().expect("file emitter lock") = Some(emitter);
    }

    /// Monotonic, never-zero request id.
    fn next_id(&self) -> u64 {
        self.next_request_id.fetch_add(1, Ordering::Relaxed) + 1
    }

    fn submit_fs(&self, op: FsOp) -> u64 {
        let id = self.next_id();
        if self
            .cmd_tx
            .send(Command::Fs { request_id: id, op })
            .is_err()
        {
            return 0;
        }
        id
    }

    /// List a directory. Empty path means the connection's home directory.
    pub fn fs_list(&self, path: String) -> u64 {
        self.submit_fs(FsOp::List(path))
    }
    /// Stat a single path (delivered as a one-element listing).
    pub fn fs_stat(&self, path: String) -> u64 {
        self.submit_fs(FsOp::Stat(path))
    }
    /// Read up to `max_len` bytes of a file.
    pub fn fs_read(&self, path: String, max_len: u64) -> u64 {
        self.submit_fs(FsOp::Read(path, max_len))
    }
    /// Overwrite a file with `data` (editor save / upload).
    pub fn fs_write(&self, path: String, data: Vec<u8>) -> u64 {
        self.submit_fs(FsOp::Write(path, data))
    }
    /// Rename / move.
    pub fn fs_rename(&self, from: String, to: String) -> u64 {
        self.submit_fs(FsOp::Rename(from, to))
    }
    /// Remove (recursively for directories when `recursive`).
    pub fn fs_remove(&self, path: String, recursive: bool) -> u64 {
        self.submit_fs(FsOp::Remove(path, recursive))
    }
    /// Create a directory.
    pub fn fs_mkdir(&self, path: String) -> u64 {
        self.submit_fs(FsOp::Mkdir(path))
    }
    /// Copy `from` to `to`.
    pub fn fs_copy(&self, from: String, to: String) -> u64 {
        self.submit_fs(FsOp::Copy(from, to))
    }

    fn send_cmd(&self, cmd: Command) -> CoreResult<()> {
        self.cmd_tx.send(cmd).map_err(|_| {
            CoreError::with_detail(RsErrorCode::RuntimeError, "session driver stopped")
        })
    }

    /// Ask the driver to shut down and block until it does (bounded). After this
    /// returns, no further events/callbacks can fire — this is what makes
    /// `rscore_session_destroy` safe.
    ///
    /// Two layers guarantee that:
    /// 1. We signal `Shutdown` + `cancel` (which interrupts any in-flight
    ///    connect/send/fs op) and join the driver within a bounded timeout.
    /// 2. Whether or not the join completed in time, we then `deactivate` every
    ///    emitter. If the driver overran the timeout and was detached, its later
    ///    `emit_*` calls become no-ops instead of touching `user_data` the C++
    ///    owner is about to free.
    pub fn shutdown_and_join(&self) {
        let _ = self.cmd_tx.send(Command::Shutdown);
        self.cancel.notify_waiters();
        let join = self.join.lock().expect("join lock").take();
        if let Some(join) = join {
            let _ = self
                .handle
                .block_on(async move { tokio::time::timeout(Duration::from_secs(3), join).await });
        }
        // Make the sinks inert regardless of whether the join succeeded.
        self.emitter.deactivate();
        if let Some(em) = self.file_emitter.lock().expect("file emitter lock").take() {
            em.deactivate();
        }
    }
}

/// Outcome of awaiting a cancellable provider operation.
enum OpOutcome {
    Done(CoreResult<()>),
    Cancelled,
}

/// The driver task: owns the provider and the state machine.
async fn drive(
    config: ConnectionConfig,
    emitter: Arc<dyn EventEmitter>,
    mut cmd_rx: mpsc::UnboundedReceiver<Command>,
    cancel: Arc<Notify>,
    gate: HostKeyGate,
    file_emitter: FileEmitterSlot,
) {
    let (prov_tx, mut prov_rx) = mpsc::unbounded_channel::<ProviderEvent>();
    let sink = ProviderSink::new(prov_tx);

    let mut provider: Box<dyn SshProvider> = match create_provider(&config) {
        Ok(p) => p,
        Err(e) => {
            emitter.emit_batch(vec![
                SessionEvent::Error {
                    code: e.code,
                    message: e.message(),
                },
                SessionEvent::StateChanged(RsSessionState::Failed),
            ]);
            return;
        }
    };
    provider.set_host_key_gate(gate);

    let mut state = RsSessionState::Idle;
    // File (SFTP) provider, obtained from the SSH provider after a successful connect.
    let mut file_provider: Option<Box<dyn FileProvider>> = None;

    loop {
        tokio::select! {
            biased;

            maybe_cmd = cmd_rx.recv() => {
                match maybe_cmd {
                    None | Some(Command::Shutdown) => {
                        if matches!(state, RsSessionState::Connected | RsSessionState::Connecting) {
                            // Bound the disconnect so a wedged network teardown can't
                            // keep the driver (and the joining destroy call) parked.
                            let _ = tokio::time::timeout(
                                Duration::from_secs(2),
                                provider.disconnect(),
                            )
                            .await;
                        }
                        break;
                    }
                    Some(Command::SetSecret(secret)) => {
                        provider.set_secret(secret);
                    }
                    Some(Command::SetPrivateKey { key_path, passphrase }) => {
                        provider.set_private_key(key_path, passphrase);
                    }
                    Some(Command::Connect) => {
                        if matches!(state, RsSessionState::Connecting | RsSessionState::Connected) {
                            emit_error(&*emitter, RsErrorCode::InvalidState, "already connecting or connected");
                            continue;
                        }
                        set_state(&*emitter, &mut state, RsSessionState::Connecting);
                        let outcome = run_cancellable(provider.connect(sink.clone()), &cancel).await;
                        match outcome {
                            OpOutcome::Done(Ok(())) => {
                                // Pick up the file (SFTP) capability if the backend opened one.
                                file_provider = provider.take_file_provider();
                                set_state(&*emitter, &mut state, RsSessionState::Connected);
                            }
                            OpOutcome::Done(Err(e)) => {
                                emit_error(&*emitter, e.code, &e.message());
                                set_state(&*emitter, &mut state, RsSessionState::Failed);
                            }
                            OpOutcome::Cancelled => {
                                let _ = provider.disconnect().await;
                                emit_error(&*emitter, RsErrorCode::Cancelled, "connection cancelled");
                                set_state(&*emitter, &mut state, RsSessionState::Disconnected);
                            }
                        }
                    }
                    Some(Command::Send(data)) => {
                        if state != RsSessionState::Connected {
                            emit_error(&*emitter, RsErrorCode::NotConnected, "cannot send while not connected");
                            continue;
                        }
                        let outcome = run_cancellable(provider.send(&data), &cancel).await;
                        match outcome {
                            OpOutcome::Done(Ok(())) => {}
                            OpOutcome::Done(Err(e)) => emit_error(&*emitter, e.code, &e.message()),
                            OpOutcome::Cancelled => {
                                emit_error(&*emitter, RsErrorCode::Cancelled, "send cancelled");
                            }
                        }
                    }
                    Some(Command::Disconnect) => {
                        if !matches!(state, RsSessionState::Connected | RsSessionState::Connecting) {
                            continue;
                        }
                        set_state(&*emitter, &mut state, RsSessionState::Disconnecting);
                        file_provider = None;
                        let _ = provider.disconnect().await;
                        set_state(&*emitter, &mut state, RsSessionState::Disconnected);
                    }
                    Some(Command::Fs { request_id, op }) => {
                        // Race the op against cancel so a long transfer can be
                        // interrupted by the cancel button or by teardown (which is
                        // what lets the driver exit promptly on shutdown).
                        let result = match run_until_cancel(
                            run_fs_op(&mut file_provider, request_id, op),
                            &cancel,
                        )
                        .await
                        {
                            Some(r) => r,
                            None => FileResult {
                                request_id,
                                payload: FilePayload::Error {
                                    code: RsErrorCode::Cancelled,
                                    message: "file operation cancelled".into(),
                                },
                            },
                        };
                        if let Some(em) = file_emitter.lock().expect("file emitter lock").clone() {
                            em.emit_file_results(vec![result]);
                        }
                    }
                }
            }

            Some(first) = prov_rx.recv() => {
                // Drain everything immediately available and deliver as ONE batch.
                let mut batch: Vec<SessionEvent> = Vec::new();
                let mut transition: Option<RsSessionState> = None;
                collect_provider_event(first, &mut batch, &mut transition);
                while let Ok(ev) = prov_rx.try_recv() {
                    collect_provider_event(ev, &mut batch, &mut transition);
                }
                if let Some(new_state) = transition {
                    state = new_state;
                    batch.push(SessionEvent::StateChanged(new_state));
                }
                if !batch.is_empty() {
                    emitter.emit_batch(batch);
                }
            }
        }
    }
}

/// Run one file operation against the (optional) file provider, producing a result.
async fn run_fs_op(
    file_provider: &mut Option<Box<dyn FileProvider>>,
    request_id: u64,
    op: FsOp,
) -> FileResult {
    let provider = match file_provider.as_mut() {
        Some(p) => p,
        None => return FileResult {
            request_id,
            payload: FilePayload::Error {
                code: RsErrorCode::ProviderUnavailable,
                message:
                    "file transfer unavailable: SFTP subsystem is not available for this session"
                        .into(),
            },
        },
    };

    fn ok_or_err(r: CoreResult<()>) -> FilePayload {
        match r {
            Ok(()) => FilePayload::Ok,
            Err(e) => FilePayload::Error {
                code: e.code,
                message: e.message(),
            },
        }
    }

    let payload = match op {
        FsOp::List(path) => match provider.list_dir(&path).await {
            Ok(entries) => FilePayload::Listing(entries),
            Err(e) => FilePayload::Error {
                code: e.code,
                message: e.message(),
            },
        },
        FsOp::Stat(path) => match provider.stat(&path).await {
            Ok(entry) => FilePayload::Listing(vec![entry]),
            Err(e) => FilePayload::Error {
                code: e.code,
                message: e.message(),
            },
        },
        FsOp::Read(path, max_len) => match provider.read_file(&path, max_len).await {
            Ok(bytes) => FilePayload::Content(bytes),
            Err(e) => FilePayload::Error {
                code: e.code,
                message: e.message(),
            },
        },
        FsOp::Write(path, data) => ok_or_err(provider.write_file(&path, &data).await),
        FsOp::Rename(from, to) => ok_or_err(provider.rename(&from, &to).await),
        FsOp::Remove(path, recursive) => ok_or_err(provider.remove(&path, recursive).await),
        FsOp::Mkdir(path) => ok_or_err(provider.mkdir(&path).await),
        FsOp::Copy(from, to) => ok_or_err(provider.copy(&from, &to).await),
    };

    FileResult {
        request_id,
        payload,
    }
}

/// Await a provider operation, racing it against the cancel signal.
async fn run_cancellable<F>(op: F, cancel: &Notify) -> OpOutcome
where
    F: std::future::Future<Output = CoreResult<()>>,
{
    tokio::pin!(op);
    tokio::select! {
        r = &mut op => OpOutcome::Done(r),
        _ = cancel.notified() => OpOutcome::Cancelled,
    }
}

/// Like [`run_cancellable`] but generic over the output: yields `Some(value)` if
/// the future finished, or `None` if cancel fired first (dropping the future and
/// thus aborting the in-flight operation).
async fn run_until_cancel<F, T>(op: F, cancel: &Notify) -> Option<T>
where
    F: std::future::Future<Output = T>,
{
    tokio::pin!(op);
    tokio::select! {
        r = &mut op => Some(r),
        _ = cancel.notified() => None,
    }
}

fn collect_provider_event(
    ev: ProviderEvent,
    batch: &mut Vec<SessionEvent>,
    transition: &mut Option<RsSessionState>,
) {
    match ev {
        ProviderEvent::Data(bytes) => batch.push(SessionEvent::Data(bytes)),
        ProviderEvent::Disconnected(_reason) => {
            *transition = Some(RsSessionState::Disconnected);
        }
        ProviderEvent::Error(e) => {
            batch.push(SessionEvent::Error {
                code: e.code,
                message: e.message(),
            });
            *transition = Some(RsSessionState::Failed);
        }
    }
}

fn set_state(emitter: &dyn EventEmitter, state: &mut RsSessionState, new_state: RsSessionState) {
    *state = new_state;
    emitter.emit_batch(vec![SessionEvent::StateChanged(new_state)]);
}

fn emit_error(emitter: &dyn EventEmitter, code: RsErrorCode, message: &str) {
    emitter.emit_batch(vec![SessionEvent::Error {
        code,
        message: message.to_string(),
    }]);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::event::RsEventKind;
    use crate::provider::RsProviderKind;
    use crate::runtime::CoreRuntime;
    use std::sync::Mutex as StdMutex;

    #[derive(Default)]
    struct CollectingEmitter {
        states: Arc<StdMutex<Vec<RsSessionState>>>,
        data: Arc<StdMutex<Vec<u8>>>,
        errors: Arc<StdMutex<Vec<RsErrorCode>>>,
    }

    impl EventEmitter for CollectingEmitter {
        fn emit_batch(&self, events: Vec<SessionEvent>) {
            for ev in events {
                match ev {
                    SessionEvent::StateChanged(s) => self.states.lock().unwrap().push(s),
                    SessionEvent::Data(mut d) => self.data.lock().unwrap().append(&mut d),
                    SessionEvent::Error { code, .. } => self.errors.lock().unwrap().push(code),
                    SessionEvent::HostKeyPrompt { .. } => {}
                }
            }
        }
    }

    fn cfg(host: &str) -> ConnectionConfig {
        ConnectionConfig {
            host: host.into(),
            port: 22,
            username: "researcher".into(),
            kind: RsProviderKind::Mock,
        }
    }

    #[test]
    fn mock_session_connects_receives_and_disconnects() {
        let rt = CoreRuntime::new().unwrap();
        let states = Arc::new(StdMutex::new(Vec::new()));
        let data = Arc::new(StdMutex::new(Vec::new()));
        let errors = Arc::new(StdMutex::new(Vec::new()));
        let emitter = Arc::new(CollectingEmitter {
            states: states.clone(),
            data: data.clone(),
            errors: errors.clone(),
        });

        let session = Session::spawn(rt.handle(), cfg("hpc.example.edu"), emitter);
        session.connect().unwrap();

        // Give the mock time to connect and emit its banner.
        rt.handle()
            .block_on(async { tokio::time::sleep(Duration::from_millis(300)).await });

        session.send(b"nvidia-smi\n".to_vec()).unwrap();
        rt.handle()
            .block_on(async { tokio::time::sleep(Duration::from_millis(200)).await });

        session.disconnect().unwrap();
        rt.handle()
            .block_on(async { tokio::time::sleep(Duration::from_millis(200)).await });

        session.shutdown_and_join();

        let states = states.lock().unwrap();
        assert!(states.contains(&RsSessionState::Connecting));
        assert!(states.contains(&RsSessionState::Connected));
        assert!(states.contains(&RsSessionState::Disconnected));
        assert!(errors.lock().unwrap().is_empty(), "no errors expected");
        let data_guard = data.lock().unwrap();
        let text = String::from_utf8_lossy(&data_guard);
        assert!(
            text.contains("hpc.example.edu"),
            "expected a banner, got: {text}"
        );
        assert!(text.contains("nvidia-smi"), "expected echoed command");
        let _ = RsEventKind::Data; // keep import meaningful
    }

    #[test]
    fn mock_session_reports_connection_failure() {
        let rt = CoreRuntime::new().unwrap();
        let states = Arc::new(StdMutex::new(Vec::new()));
        let errors = Arc::new(StdMutex::new(Vec::new()));
        let emitter = Arc::new(CollectingEmitter {
            states: states.clone(),
            data: Arc::new(StdMutex::new(Vec::new())),
            errors: errors.clone(),
        });

        // The mock treats hosts starting with "fail" as unreachable.
        let session = Session::spawn(rt.handle(), cfg("fail.example.edu"), emitter);
        session.connect().unwrap();
        rt.handle()
            .block_on(async { tokio::time::sleep(Duration::from_millis(300)).await });
        session.shutdown_and_join();

        assert!(states.lock().unwrap().contains(&RsSessionState::Failed));
        assert!(errors.lock().unwrap().contains(&RsErrorCode::ConnectFailed));
    }
}
