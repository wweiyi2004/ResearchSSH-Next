//! Real SSH backend built on the [`russh`](https://docs.rs/russh) crate.
//!
//! Compiled only with `--features russh`. Implements an interactive shell:
//! TCP+SSH handshake, host-key verification (known_hosts + a UI confirm prompt for
//! unknown hosts), password authentication, a PTY + shell, and bulk bidirectional
//! data streaming. It also attempts to open an SFTP subsystem for file
//! management. Errors map to [`CoreError`] with precise [`RsErrorCode`]s.

use crate::fs::{looks_like_text, FileEntry, FileKind, FileProvider};
use crate::provider::{ConnectionConfig, ProviderEvent, ProviderSink, SshProvider};
use crate::secret::Secret;
use crate::session::HostKeyGate;
use crate::{CoreError, CoreResult, RsErrorCode};
use async_trait::async_trait;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::task::JoinHandle;

use russh::client::{self, Handle};
use russh::keys::{HashAlg, PrivateKey, PrivateKeyWithHashAlg, PublicKey};
use russh::Disconnect;
use russh_sftp::client::SftpSession;

/// russh client handler: only host-key verification needs custom logic.
struct ClientHandler {
    host: String,
    port: u16,
    gate: Option<HostKeyGate>,
}

impl client::Handler for ClientHandler {
    type Error = russh::Error;

    async fn check_server_key(
        &mut self,
        server_public_key: &PublicKey,
    ) -> Result<bool, Self::Error> {
        let fingerprint = server_public_key.fingerprint(HashAlg::Sha256).to_string();

        // Consult the user's known_hosts first.
        match russh::keys::check_known_hosts(&self.host, self.port, server_public_key) {
            Ok(true) => return Ok(true), // known host, key matches
            Ok(false) => {}              // unknown host → prompt below
            Err(_) => return Ok(false),  // key changed / parse error → reject
        }

        // Unknown host: ask the UI to confirm the fingerprint.
        let accept = match &self.gate {
            Some(gate) => {
                gate.confirm(format!("{fingerprint}  ({}:{})", self.host, self.port))
                    .await
            }
            None => false,
        };
        if accept {
            let _ = learn_known_hosts(&self.host, self.port, server_public_key);
        }
        Ok(accept)
    }
}

/// Append an accepted host key to `~/.ssh/known_hosts` (trust-on-first-use).
/// `russh::keys` exposes `check_known_hosts` but no writer, so we append a normal
/// OpenSSH known_hosts line ourselves.
fn learn_known_hosts(host: &str, port: u16, key: &PublicKey) -> std::io::Result<()> {
    use std::io::{Error, ErrorKind, Write};
    let home = std::env::var_os("USERPROFILE")
        .or_else(|| std::env::var_os("HOME"))
        .ok_or_else(|| Error::new(ErrorKind::NotFound, "no home directory"))?;
    let mut path = std::path::PathBuf::from(home);
    path.push(".ssh");
    std::fs::create_dir_all(&path)?;
    path.push("known_hosts");

    let host_field = if port == 22 {
        host.to_string()
    } else {
        format!("[{host}]:{port}")
    };
    let key_openssh = key
        .to_openssh()
        .map_err(|e| Error::new(ErrorKind::InvalidData, e.to_string()))?;

    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)?;
    writeln!(file, "{host_field} {key_openssh}")
}

/// Real SSH provider. See module docs.
pub struct RusshProvider {
    config: ConnectionConfig,
    secret: Option<Secret>,
    key_path: Option<String>,
    key_passphrase: Option<Secret>,
    gate: Option<HostKeyGate>,
    session: Option<Handle<ClientHandler>>,
    writer: Option<Box<dyn AsyncWrite + Send + Unpin>>,
    read_task: Option<JoinHandle<()>>,
    file_provider: Option<Box<dyn FileProvider>>,
}

impl RusshProvider {
    /// Create a russh-backed provider for the given connection config.
    pub fn new(config: ConnectionConfig) -> Self {
        Self {
            config,
            secret: None,
            key_path: None,
            key_passphrase: None,
            gate: None,
            session: None,
            writer: None,
            read_task: None,
            file_provider: None,
        }
    }
}

fn connect_err(stage: &str, e: impl std::fmt::Display) -> CoreError {
    CoreError::with_detail(RsErrorCode::ConnectFailed, format!("{stage}: {e}"))
}

/// Try public-key auth against `session` for every candidate key, in order.
/// Returns `true` on the first key the server accepts. Per-key load/auth failures
/// are swallowed so the next candidate is tried; the caller falls back to password.
async fn try_publickey_auth(
    session: &mut Handle<ClientHandler>,
    username: &str,
    candidates: &[PathBuf],
    passphrase: Option<&str>,
) -> bool {
    for path in candidates {
        let key: PrivateKey = match russh::keys::load_secret_key(path, passphrase) {
            Ok(k) => k,
            Err(_) => continue, // wrong passphrase / unreadable / unparseable
        };
        // For non-RSA keys `new` resets the hash to None; Some(Sha256) is the
        // correct choice for RSA (rsa-sha2-256).
        let kwh = PrivateKeyWithHashAlg::new(Arc::new(key), Some(HashAlg::Sha256));
        match session.authenticate_publickey(username, kwh).await {
            Ok(result) if result.success() => return true,
            _ => continue,
        }
    }
    false
}

#[async_trait]
impl SshProvider for RusshProvider {
    fn set_secret(&mut self, secret: Secret) {
        self.secret = Some(secret);
    }

    fn set_private_key(&mut self, key_path: Option<String>, passphrase: Option<Secret>) {
        self.key_path = key_path;
        self.key_passphrase = passphrase;
    }

    fn set_host_key_gate(&mut self, gate: HostKeyGate) {
        self.gate = Some(gate);
    }

    fn take_file_provider(&mut self) -> Option<Box<dyn FileProvider>> {
        self.file_provider.take()
    }

    async fn connect(&mut self, sink: ProviderSink) -> CoreResult<()> {
        let handler = ClientHandler {
            host: self.config.host.clone(),
            port: self.config.port,
            gate: self.gate.clone(),
        };

        let config = Arc::new(client::Config::default());
        let mut session = client::connect(
            config,
            (self.config.host.as_str(), self.config.port),
            handler,
        )
        .await
        .map_err(|e| connect_err("connect", e))?;

        // Public-key first: an explicit key path overrides auto-discovery.
        let candidates: Vec<PathBuf> = match &self.key_path {
            Some(p) => vec![PathBuf::from(p)],
            None => default_ssh_dir()
                .map(|d| keys_in_dir(&d))
                .unwrap_or_default(),
        };
        let passphrase: Option<String> = match &self.key_passphrase {
            Some(s) => Some(String::from_utf8(s.expose().to_vec()).map_err(|_| {
                CoreError::with_detail(
                    RsErrorCode::ConnectFailed,
                    "key passphrase is not valid UTF-8",
                )
            })?),
            None => None,
        };
        let mut authenticated = false;
        if !candidates.is_empty() {
            authenticated = try_publickey_auth(
                &mut session,
                &self.config.username,
                &candidates,
                passphrase.as_deref(),
            )
            .await;
        }

        // Fall back to password auth if public key did not succeed.
        if !authenticated {
            if let Some(secret) = &self.secret {
                let password = String::from_utf8(secret.expose().to_vec()).map_err(|_| {
                    CoreError::with_detail(
                        RsErrorCode::ConnectFailed,
                        "password is not valid UTF-8",
                    )
                })?;
                let auth = session
                    .authenticate_password(&self.config.username, password)
                    .await
                    .map_err(|e| connect_err("auth", e))?;
                authenticated = auth.success();
            }
        }

        if !authenticated {
            let detail = if candidates.is_empty() && self.secret.is_none() {
                "no usable key or password"
            } else {
                "authentication failed (public-key and password)"
            };
            return Err(CoreError::with_detail(RsErrorCode::ConnectFailed, detail));
        }

        // Open a session channel and request an interactive PTY + shell.
        let channel = session
            .channel_open_session()
            .await
            .map_err(|e| connect_err("open channel", e))?;
        channel
            .request_pty(false, "xterm-256color", 80, 24, 0, 0, &[])
            .await
            .map_err(|e| connect_err("request pty", e))?;
        channel
            .request_shell(true)
            .await
            .map_err(|e| connect_err("request shell", e))?;

        // Split the channel into a reader (background task → sink) and a writer.
        let (mut reader, writer) = tokio::io::split(channel.into_stream());
        let read_sink = sink.clone();
        let read_task = tokio::spawn(async move {
            let mut buf = vec![0u8; 16 * 1024];
            loop {
                match reader.read(&mut buf).await {
                    Ok(0) => {
                        read_sink.emit(ProviderEvent::Disconnected("connection closed".into()));
                        break;
                    }
                    Ok(n) => read_sink.emit(ProviderEvent::Data(buf[..n].to_vec())),
                    Err(e) => {
                        read_sink.emit(ProviderEvent::Error(CoreError::with_detail(
                            RsErrorCode::RuntimeError,
                            format!("read: {e}"),
                        )));
                        break;
                    }
                }
            }
        });

        // Best-effort: open an SFTP subsystem on a second channel for file ops.
        // If it fails (or the server has no SFTP subsystem) the shell still works;
        // file transfer is simply unavailable. Bounded by a timeout so a server that
        // never answers the subsystem request can't stall the connection.
        let open_sftp = async {
            let channel = session.channel_open_session().await.ok()?;
            channel.request_subsystem(true, "sftp").await.ok()?;
            SftpSession::new(channel.into_stream()).await.ok()
        };
        if let Ok(Some(sftp)) =
            tokio::time::timeout(std::time::Duration::from_secs(2), open_sftp).await
        {
            self.file_provider = Some(Box::new(SftpFileProvider::new(sftp)));
        }

        self.session = Some(session);
        self.writer = Some(Box::new(writer));
        self.read_task = Some(read_task);
        Ok(())
    }

    async fn send(&mut self, data: &[u8]) -> CoreResult<()> {
        let writer = self
            .writer
            .as_mut()
            .ok_or_else(|| CoreError::new(RsErrorCode::NotConnected))?;
        writer.write_all(data).await.map_err(|e| {
            CoreError::with_detail(RsErrorCode::RuntimeError, format!("write: {e}"))
        })?;
        writer.flush().await.map_err(|e| {
            CoreError::with_detail(RsErrorCode::RuntimeError, format!("flush: {e}"))
        })?;
        Ok(())
    }

    async fn disconnect(&mut self) -> CoreResult<()> {
        if let Some(mut writer) = self.writer.take() {
            let _ = writer.shutdown().await;
        }
        if let Some(task) = self.read_task.take() {
            task.abort();
        }
        if let Some(session) = self.session.take() {
            let _ = session
                .disconnect(Disconnect::ByApplication, "", "en")
                .await;
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// SFTP-backed file provider (real file operations over the russh connection).
// ---------------------------------------------------------------------------

/// File provider backed by an SFTP subsystem on the russh session.
pub struct SftpFileProvider {
    sftp: SftpSession,
    home: Option<String>,
}

impl SftpFileProvider {
    fn new(sftp: SftpSession) -> Self {
        Self { sftp, home: None }
    }

    async fn home_dir(&mut self) -> String {
        if let Some(home) = &self.home {
            return home.clone();
        }
        let home = self
            .sftp
            .canonicalize(".")
            .await
            .unwrap_or_else(|_| ".".to_string());
        self.home = Some(home.clone());
        home
    }

    /// Resolve an empty/`.` or relative directory to the connection's home directory.
    async fn resolve_dir(&mut self, path: &str) -> String {
        if !path.is_empty() && path != "." && path.starts_with('/') {
            return path.to_string();
        }
        let home = self.home_dir().await;
        if path.is_empty() || path == "." {
            home
        } else {
            join_path(&home, path)
        }
    }

    /// Resolve an empty/`.` or relative file path to the connection's home directory.
    async fn resolve_path(&mut self, path: &str) -> String {
        if path.is_empty() || path == "." {
            return self.resolve_dir(path).await;
        }
        if path.starts_with('/') {
            return path.to_string();
        }
        let home = self.home_dir().await;
        join_path(&home, path)
    }
}

fn sftp_err(stage: &str, e: impl std::fmt::Display) -> CoreError {
    let raw = e.to_string();
    let lower = raw.to_ascii_lowercase();
    let reason = if lower.contains("permission") || lower.contains("denied") {
        "permission denied"
    } else if lower.contains("no such") || lower.contains("not found") {
        "path not found"
    } else if lower.contains("exists") {
        "target already exists"
    } else if lower.contains("directory not empty") || lower.contains("not empty") {
        "directory not empty"
    } else {
        raw.as_str()
    };
    let message = if reason == raw.as_str() {
        format!("sftp {stage}: {raw}")
    } else {
        format!("sftp {stage}: {reason} ({raw})")
    };
    CoreError::with_detail(RsErrorCode::RuntimeError, message)
}

fn join_path(dir: &str, name: &str) -> String {
    format!("{}/{}", dir.trim_end_matches('/'), name)
}

/// The user's ~/.ssh directory from USERPROFILE (Windows) or HOME, if set.
fn default_ssh_dir() -> Option<PathBuf> {
    std::env::var_os("USERPROFILE")
        .or_else(|| std::env::var_os("HOME"))
        .map(|home| PathBuf::from(home).join(".ssh"))
}

/// Default OpenSSH identity files in `ssh_dir` that actually exist, in OpenSSH
/// preference order (modern algorithms first).
fn keys_in_dir(ssh_dir: &Path) -> Vec<PathBuf> {
    ["id_ed25519", "id_ecdsa", "id_rsa"]
        .iter()
        .map(|name| ssh_dir.join(name))
        .filter(|p| p.is_file())
        .collect()
}

/// Classify an SFTP entry, distinguishing symlinks from real directories/files.
/// Whether `meta` came from STAT or LSTAT determines whether a symlink shows up as
/// `Symlink` (LSTAT, the link itself) or as its target's kind (STAT, followed).
fn map_meta_kind(meta: &russh_sftp::protocol::FileAttributes) -> FileKind {
    if meta.is_symlink() {
        FileKind::Symlink
    } else if meta.is_dir() {
        FileKind::Directory
    } else if meta.is_regular() {
        FileKind::File
    } else {
        FileKind::Other
    }
}

#[async_trait]
impl FileProvider for SftpFileProvider {
    async fn list_dir(&mut self, path: &str) -> CoreResult<Vec<FileEntry>> {
        let dir = self.resolve_dir(path).await;
        let read_dir = self
            .sftp
            .read_dir(dir.clone())
            .await
            .map_err(|e| sftp_err("read_dir", e))?;
        let mut out = Vec::new();
        for entry in read_dir {
            let name = entry.file_name();
            if name == "." || name == ".." {
                continue;
            }
            // `read_dir` carries each entry's own attributes (LSTAT semantics), so a
            // symlink is reported as `Symlink`, not as whatever it points at.
            let meta = entry.metadata();
            let kind = map_meta_kind(&meta);
            out.push(FileEntry {
                path: join_path(&dir, &name),
                kind,
                size: meta.size.unwrap_or(0),
                modified_unix: meta.mtime.map(|m| m as i64).unwrap_or(-1),
                mode: meta.permissions.unwrap_or(0),
                editable_text: kind == FileKind::File && looks_like_text(&name),
                name,
            });
        }
        Ok(out)
    }

    async fn stat(&mut self, path: &str) -> CoreResult<FileEntry> {
        let p = self.resolve_path(path).await;
        let meta = self
            .sftp
            .metadata(p.clone())
            .await
            .map_err(|e| sftp_err("metadata", e))?;
        let name = p.rsplit('/').next().unwrap_or(&p).to_string();
        // `metadata` follows symlinks, so an explicit stat reports the target's kind.
        let kind = map_meta_kind(&meta);
        Ok(FileEntry {
            kind,
            size: meta.size.unwrap_or(0),
            modified_unix: meta.mtime.map(|m| m as i64).unwrap_or(-1),
            mode: meta.permissions.unwrap_or(0),
            editable_text: kind == FileKind::File && looks_like_text(&name),
            name,
            path: p,
        })
    }

    async fn read_file(&mut self, path: &str, max_len: u64) -> CoreResult<Vec<u8>> {
        let p = self.resolve_path(path).await;
        let file = self.sftp.open(p).await.map_err(|e| sftp_err("open", e))?;
        let mut data = Vec::new();
        if max_len > 0 {
            // Bounded read: pull at most `max_len` bytes so opening a huge remote
            // file (e.g. a multi-GB log) can't OOM the core. Previously the whole
            // file was read into memory and only then truncated.
            file.take(max_len)
                .read_to_end(&mut data)
                .await
                .map_err(|e| sftp_err("read", e))?;
        } else {
            // `max_len == 0` means "no cap" (callers that genuinely want everything).
            let mut file = file;
            file.read_to_end(&mut data)
                .await
                .map_err(|e| sftp_err("read", e))?;
        }
        Ok(data)
    }

    async fn write_file(&mut self, path: &str, data: &[u8]) -> CoreResult<()> {
        let p = self.resolve_path(path).await;
        self.sftp
            .write(p, data)
            .await
            .map_err(|e| sftp_err("write", e))
    }

    async fn rename(&mut self, from: &str, to: &str) -> CoreResult<()> {
        let from = self.resolve_path(from).await;
        let to = self.resolve_path(to).await;
        self.sftp
            .rename(from, to)
            .await
            .map_err(|e| sftp_err("rename", e))
    }

    async fn remove(&mut self, path: &str, recursive: bool) -> CoreResult<()> {
        let path = self.resolve_path(path).await;
        // LSTAT, NOT stat: a delete must act on the path itself and never follow a
        // symlink. Following it here previously let a recursive delete of a symlink
        // that pointed at a directory wipe out the *target* directory's contents.
        let meta = self
            .sftp
            .symlink_metadata(path.clone())
            .await
            .map_err(|e| sftp_err("stat", e))?;
        // Only a real directory (a symlink to a directory is not one) is descended.
        if meta.is_dir() && !meta.is_symlink() {
            if recursive {
                let read_dir = self
                    .sftp
                    .read_dir(path.clone())
                    .await
                    .map_err(|e| sftp_err("read_dir", e))?;
                for entry in read_dir {
                    let name = entry.file_name();
                    if name == "." || name == ".." {
                        continue;
                    }
                    self.remove(&join_path(&path, &name), true).await?;
                }
            }
            self.sftp
                .remove_dir(path)
                .await
                .map_err(|e| sftp_err("rmdir", e))
        } else {
            // Regular file, symlink, or other: a single unlink. `remove_file` removes
            // a symlink itself without dereferencing it.
            self.sftp
                .remove_file(path)
                .await
                .map_err(|e| sftp_err("remove", e))
        }
    }

    async fn mkdir(&mut self, path: &str) -> CoreResult<()> {
        let p = self.resolve_path(path).await;
        self.sftp
            .create_dir(p)
            .await
            .map_err(|e| sftp_err("mkdir", e))
    }

    async fn copy(&mut self, from: &str, to: &str) -> CoreResult<()> {
        let from = self.resolve_path(from).await;
        let to = self.resolve_path(to).await;
        let data = self
            .sftp
            .read(from)
            .await
            .map_err(|e| sftp_err("copy read", e))?;
        self.sftp
            .write(to, &data)
            .await
            .map_err(|e| sftp_err("copy write", e))
    }
}

#[cfg(test)]
mod discovery_tests {
    use super::*;

    #[test]
    fn keys_in_dir_returns_existing_in_preference_order() {
        let dir = std::env::temp_dir().join(format!("rssh_disc_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        // Create id_rsa and id_ed25519 but NOT id_ecdsa.
        std::fs::write(dir.join("id_rsa"), b"x").unwrap();
        std::fs::write(dir.join("id_ed25519"), b"x").unwrap();

        let found = keys_in_dir(&dir);
        let names: Vec<String> = found
            .iter()
            .map(|p| p.file_name().unwrap().to_string_lossy().into_owned())
            .collect();
        assert_eq!(names, vec!["id_ed25519".to_string(), "id_rsa".to_string()]);

        let _ = std::fs::remove_dir_all(&dir);
    }
}

#[cfg(test)]
mod kind_tests {
    use super::*;
    use russh_sftp::protocol::FileAttributes;

    fn attrs() -> FileAttributes {
        FileAttributes {
            size: None,
            uid: None,
            user: None,
            gid: None,
            group: None,
            permissions: None,
            atime: None,
            mtime: None,
        }
    }

    // A symlink must classify as `Symlink`, never `Directory` — that is what keeps a
    // recursive delete from descending through a link into its target. The ordering
    // matters because SFTP's S_IFLNK shares bits with S_IFREG, so `is_regular()` is
    // also true for a symlink; `map_meta_kind` checks `is_symlink()` first.
    #[test]
    fn symlink_is_classified_before_dir_or_regular() {
        let mut link = attrs();
        link.set_symlink(true);
        assert!(link.is_symlink());
        assert_eq!(map_meta_kind(&link), FileKind::Symlink);

        let mut dir = attrs();
        dir.set_dir(true);
        assert_eq!(map_meta_kind(&dir), FileKind::Directory);

        let mut reg = attrs();
        reg.set_regular(true);
        assert_eq!(map_meta_kind(&reg), FileKind::File);
    }
}

// ---------------------------------------------------------------------------
// End-to-end test: an in-process russh SERVER + the real RusshProvider driven
// through the C ABI. A genuine SSH handshake/auth/shell over loopback — no
// external server needed.
// ---------------------------------------------------------------------------
#[cfg(test)]
mod e2e {
    // This test module marshals raw pointers across the C ABI (like ffi.rs), so it
    // needs `unsafe`; the rest of the crate keeps `#![deny(unsafe_code)]`.
    #![allow(unsafe_code)]

    use crate::event::{RsEvent, RsEventKind, RsSessionState};
    use crate::ffi::{
        rscore_create, rscore_destroy, rscore_session_confirm_host_key, rscore_session_connect,
        rscore_session_create, rscore_session_destroy, rscore_session_disconnect,
        rscore_session_send, rscore_session_set_password, rscore_session_set_private_key, RsSession,
        RsSessionConfig,
    };
    use crate::provider::RsProviderKind;
    use crate::RsErrorCode;
    use russh::keys::ssh_key::LineEnding;
    use russh::keys::{PrivateKey, PublicKey};
    use std::ffi::{c_void, CString};
    use std::sync::atomic::{AtomicPtr, Ordering};
    use std::sync::Mutex;
    use std::time::Duration;

    use russh::server::{self, Auth, Msg, Server as _, Session};
    use russh::{Channel, ChannelId, Pty};

    // ----- in-process test server -----
    struct Srv {
        trusted: Option<PublicKey>,
    }
    impl server::Server for Srv {
        type Handler = ShellHandler;
        fn new_client(&mut self, _peer: Option<std::net::SocketAddr>) -> ShellHandler {
            ShellHandler {
                trusted: self.trusted.clone(),
            }
        }
    }

    struct ShellHandler {
        trusted: Option<PublicKey>,
    }
    impl server::Handler for ShellHandler {
        type Error = russh::Error;

        async fn auth_password(&mut self, user: &str, password: &str) -> Result<Auth, Self::Error> {
            if user == "researcher" && password == "secret" {
                Ok(Auth::Accept)
            } else {
                Ok(Auth::reject())
            }
        }

        async fn auth_publickey(
            &mut self,
            _user: &str,
            key: &PublicKey,
        ) -> Result<Auth, Self::Error> {
            match &self.trusted {
                Some(t) if key == t => Ok(Auth::Accept),
                _ => Ok(Auth::reject()),
            }
        }

        async fn channel_open_session(
            &mut self,
            _channel: Channel<Msg>,
            _session: &mut Session,
        ) -> Result<bool, Self::Error> {
            Ok(true)
        }

        async fn pty_request(
            &mut self,
            _channel: ChannelId,
            _term: &str,
            _col: u32,
            _row: u32,
            _pw: u32,
            _ph: u32,
            _modes: &[(Pty, u32)],
            _session: &mut Session,
        ) -> Result<(), Self::Error> {
            Ok(())
        }

        async fn shell_request(
            &mut self,
            channel: ChannelId,
            session: &mut Session,
        ) -> Result<(), Self::Error> {
            let banner = b"Welcome to the in-process test SSH server\r\nresearcher@test:~$ ";
            let _ = session.data(channel, banner.to_vec());
            Ok(())
        }

        async fn data(
            &mut self,
            channel: ChannelId,
            data: &[u8],
            session: &mut Session,
        ) -> Result<(), Self::Error> {
            let mut out = b"echo: ".to_vec();
            out.extend_from_slice(data);
            let _ = session.data(channel, out);
            Ok(())
        }
    }

    // ----- FFI event collector -----
    #[derive(Default)]
    struct Collector {
        states: Mutex<Vec<RsSessionState>>,
        text: Mutex<String>,
        errors: Mutex<Vec<RsErrorCode>>,
        session: AtomicPtr<RsSession>,
    }

    extern "C" fn collect(user_data: *mut c_void, events: *const RsEvent, count: usize) {
        let c = unsafe { &*(user_data as *const Collector) };
        let evs = unsafe { std::slice::from_raw_parts(events, count) };
        for ev in evs {
            match ev.kind {
                RsEventKind::StateChanged => c.states.lock().unwrap().push(ev.state),
                RsEventKind::Data => {
                    let d = unsafe { std::slice::from_raw_parts(ev.data, ev.data_len) };
                    c.text.lock().unwrap().push_str(&String::from_utf8_lossy(d));
                }
                RsEventKind::Error => c.errors.lock().unwrap().push(ev.error_code),
                RsEventKind::HostKeyPrompt => {
                    // Auto-accept the unknown host key (as the UI dialog would).
                    let s = c.session.load(Ordering::SeqCst);
                    if !s.is_null() {
                        rscore_session_confirm_host_key(s, true);
                    }
                }
            }
        }
    }

    #[test]
    fn russh_provider_real_handshake_over_loopback() {
        // Keep known_hosts writes out of the real ~/.ssh during the test.
        let tmp = std::env::temp_dir().join(format!("rssh_e2e_{}", std::process::id()));
        std::fs::create_dir_all(&tmp).unwrap();
        std::env::set_var("USERPROFILE", &tmp);
        std::env::set_var("HOME", &tmp);

        // Start the in-process SSH server on an ephemeral loopback port.
        let server_rt = tokio::runtime::Runtime::new().unwrap();
        let port = server_rt.block_on(async {
            let listener = tokio::net::TcpListener::bind(("127.0.0.1", 0u16))
                .await
                .unwrap();
            let port = listener.local_addr().unwrap().port();
            let host_key =
                russh::keys::PrivateKey::random(&mut rand::rng(), russh::keys::Algorithm::Ed25519)
                    .unwrap();
            let config = std::sync::Arc::new(server::Config {
                keys: vec![host_key],
                ..Default::default()
            });
            tokio::spawn(async move {
                let mut srv = Srv { trusted: None };
                let _ = srv.run_on_socket(config, &listener).await;
            });
            port
        });

        // Drive the real provider through the C ABI.
        let collector = Box::new(Collector::default());
        let cptr = &*collector as *const Collector as *mut c_void;

        let mut err = RsErrorCode::Internal;
        let core = rscore_create(&mut err);
        assert!(!core.is_null());

        let host = CString::new("127.0.0.1").unwrap();
        let user = CString::new("researcher").unwrap();
        let config = RsSessionConfig {
            host: host.as_ptr(),
            port,
            username: user.as_ptr(),
            provider: RsProviderKind::Russh,
        };
        let session = rscore_session_create(core, &config, Some(collect), cptr, &mut err);
        assert!(!session.is_null(), "session create failed: {err:?}");
        collector.session.store(session, Ordering::SeqCst);

        let pw = b"secret";
        assert_eq!(
            rscore_session_set_password(session, pw.as_ptr(), pw.len()),
            RsErrorCode::Ok
        );
        assert_eq!(rscore_session_connect(session), RsErrorCode::Ok);

        // Wait for handshake + banner (connect also spends up to ~2s probing for an
        // SFTP subsystem the shell-only test server doesn't provide).
        std::thread::sleep(Duration::from_millis(3200));
        {
            let states = collector.states.lock().unwrap();
            assert!(
                states.contains(&RsSessionState::Connected),
                "expected Connected, states={states:?}, errors={:?}, text={:?}",
                collector.errors.lock().unwrap(),
                collector.text.lock().unwrap()
            );
        }

        // Send a command; the server echoes it.
        let cmd = b"hello\n";
        assert_eq!(
            rscore_session_send(session, cmd.as_ptr(), cmd.len()),
            RsErrorCode::Ok
        );
        std::thread::sleep(Duration::from_millis(600));

        rscore_session_disconnect(session);
        std::thread::sleep(Duration::from_millis(300));
        rscore_session_destroy(session);
        rscore_destroy(core);

        let text = collector.text.lock().unwrap();
        assert!(text.contains("test SSH server"), "banner missing: {text:?}");
        assert!(text.contains("echo: hello"), "echo missing: {text:?}");

        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn russh_provider_publickey_auth_over_loopback() {
        let tmp = std::env::temp_dir().join(format!("rssh_pk_{}", std::process::id()));
        std::fs::create_dir_all(&tmp).unwrap();
        std::env::set_var("USERPROFILE", &tmp);
        std::env::set_var("HOME", &tmp);

        // Client key pair; the server will trust the public half.
        let client_key =
            PrivateKey::random(&mut rand::rng(), russh::keys::Algorithm::Ed25519).unwrap();
        let trusted = client_key.public_key().clone();
        let key_path = tmp.join("id_ed25519_test");
        client_key
            .write_openssh_file(&key_path, LineEnding::LF)
            .unwrap();

        let server_rt = tokio::runtime::Runtime::new().unwrap();
        let port = server_rt.block_on(async {
            let listener = tokio::net::TcpListener::bind(("127.0.0.1", 0u16))
                .await
                .unwrap();
            let port = listener.local_addr().unwrap().port();
            let host_key =
                russh::keys::PrivateKey::random(&mut rand::rng(), russh::keys::Algorithm::Ed25519)
                    .unwrap();
            let config = std::sync::Arc::new(server::Config {
                keys: vec![host_key],
                ..Default::default()
            });
            tokio::spawn(async move {
                let mut srv = Srv {
                    trusted: Some(trusted),
                };
                let _ = srv.run_on_socket(config, &listener).await;
            });
            port
        });

        let collector = Box::new(Collector::default());
        let cptr = &*collector as *const Collector as *mut c_void;
        let mut err = RsErrorCode::Internal;
        let core = rscore_create(&mut err);
        let host = CString::new("127.0.0.1").unwrap();
        let user = CString::new("researcher").unwrap();
        let config = RsSessionConfig {
            host: host.as_ptr(),
            port,
            username: user.as_ptr(),
            provider: RsProviderKind::Russh,
        };
        let session = rscore_session_create(core, &config, Some(collect), cptr, &mut err);
        assert!(!session.is_null());
        collector.session.store(session, Ordering::SeqCst);

        // Explicit key path; no password set — authentication must use the key.
        let key_path_c = CString::new(key_path.to_string_lossy().as_ref()).unwrap();
        assert_eq!(
            rscore_session_set_private_key(session, key_path_c.as_ptr(), std::ptr::null(), 0),
            RsErrorCode::Ok
        );
        assert_eq!(rscore_session_connect(session), RsErrorCode::Ok);
        std::thread::sleep(Duration::from_millis(3200));
        {
            let states = collector.states.lock().unwrap();
            assert!(
                states.contains(&RsSessionState::Connected),
                "expected Connected via public key, states={states:?}, errors={:?}",
                collector.errors.lock().unwrap()
            );
        }
        rscore_session_disconnect(session);
        std::thread::sleep(Duration::from_millis(300));
        rscore_session_destroy(session);
        rscore_destroy(core);
        let _ = std::fs::remove_dir_all(&tmp);
    }
}
