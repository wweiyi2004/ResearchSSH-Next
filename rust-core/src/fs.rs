//! Remote file management (SFTP-like) abstraction.
//!
//! This module defines the operations used by the remote file tree and in-app
//! editor: browse, copy/paste, rename/delete, upload, read and save. Concrete
//! implementations live with the SSH providers: the Mock backend ships an
//! in-memory filesystem, and the `russh` backend uses `russh-sftp` when the
//! server exposes an SFTP subsystem.
//!
//! The matching C ABI, C++ models and QML are specified in
//! `docs/file-management.md`. The layering rules are unchanged: this lives in the
//! Rust core, is reached only through the FFI, and never lets the UI touch raw
//! handles or secrets.
//!
//! # How it fits the existing design
//! A session owns an [`SshProvider`](crate::provider::SshProvider) for the
//! interactive shell. File management is a *second* capability on the same
//! connection: a provider can hand a `Box<dyn FileProvider>` to the session after
//! connect via [`SshProvider::take_file_provider`](crate::provider::SshProvider::take_file_provider).

use crate::provider::RsProviderKind;
use crate::{CoreError, CoreResult, RsErrorCode};
use async_trait::async_trait;

/// Kind of a directory entry. Numeric values match the C ABI (`RsFileKind`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileKind {
    /// A regular file.
    File = 0,
    /// A directory.
    Directory = 1,
    /// A symbolic link.
    Symlink = 2,
    /// Anything else (socket, device, …).
    Other = 3,
}

/// One entry in a remote directory listing.
///
/// At the FFI boundary this becomes a `#[repr(C)] RsFileEntry` whose string
/// pointers are valid only for the duration of the result callback (the C++ side
/// copies them out), mirroring the [`RsEvent`](crate::event::RsEvent) contract.
#[derive(Debug, Clone)]
pub struct FileEntry {
    /// Base name (no path), UTF-8.
    pub name: String,
    /// Full remote path, UTF-8 (POSIX `/`-separated).
    pub path: String,
    /// Entry kind.
    pub kind: FileKind,
    /// Size in bytes (0 for directories).
    pub size: u64,
    /// Last-modified time, seconds since the Unix epoch; `-1` if unknown.
    pub modified_unix: i64,
    /// POSIX permission/mode bits.
    pub mode: u32,
    /// Heuristic: whether the in-app editor should offer to open this as text
    /// (see [`looks_like_text`]).
    pub editable_text: bool,
}

/// Remote file operations (SFTP-like). The real implementation runs each method
/// as an SFTP request over the session's channel.
///
/// All methods are async and `Send` so they run on the session driver task. Bulk
/// data (`read_file` / `write_file`) crosses the boundary as one buffer — never
/// chunk-per-byte. Errors map to [`CoreError`] with a precise [`RsErrorCode`].
#[async_trait]
pub trait FileProvider: Send {
    /// List the entries of `path` (one directory level; the tree lazy-loads).
    async fn list_dir(&mut self, path: &str) -> CoreResult<Vec<FileEntry>>;

    /// Stat a single path.
    async fn stat(&mut self, path: &str) -> CoreResult<FileEntry>;

    /// Read up to `max_len` bytes of a file (the editor caps text files).
    async fn read_file(&mut self, path: &str, max_len: u64) -> CoreResult<Vec<u8>>;

    /// Overwrite `path` with `data` (used by editor "save" and by uploads).
    async fn write_file(&mut self, path: &str, data: &[u8]) -> CoreResult<()>;

    /// Rename / move `from` to `to`.
    async fn rename(&mut self, from: &str, to: &str) -> CoreResult<()>;

    /// Remove `path` (recursively for directories when `recursive`).
    async fn remove(&mut self, path: &str, recursive: bool) -> CoreResult<()>;

    /// Create a directory.
    async fn mkdir(&mut self, path: &str) -> CoreResult<()>;

    /// Copy `from` to `to` (the SFTP backend realises this as read+write or a
    /// server-side `cp`).
    async fn copy(&mut self, from: &str, to: &str) -> CoreResult<()>;
}

/// Result of a single file operation, delivered (with the `request_id` returned
/// by the FFI call that started it) to the session's file callback.
#[derive(Debug)]
pub struct FileResult {
    /// Correlates with the request id returned by the `rscore_session_fs_*` call.
    pub request_id: u64,
    /// The outcome.
    pub payload: FilePayload,
}

/// Payload of a [`FileResult`].
#[derive(Debug)]
pub enum FilePayload {
    /// A directory listing (also used for a single stat — a one-element listing).
    Listing(Vec<FileEntry>),
    /// File contents (from a read).
    Content(Vec<u8>),
    /// A mutating op (write/rename/remove/mkdir/copy) succeeded.
    Ok,
    /// The op failed.
    Error {
        /// Stable error code.
        code: RsErrorCode,
        /// UTF-8 detail message.
        message: String,
    },
}

/// Reserved standalone factory for the file provider.
///
/// Active sessions get file capability from their SSH provider via
/// [`SshProvider::take_file_provider`](crate::provider::SshProvider::take_file_provider).
/// This factory remains unavailable because a real SFTP provider must be created
/// from an already-authenticated SSH session/channel rather than only a provider
/// kind.
pub fn create_file_provider(kind: RsProviderKind) -> CoreResult<Box<dyn FileProvider>> {
    let _ = kind;
    Err(CoreError::with_detail(
        RsErrorCode::ProviderUnavailable,
        "standalone file provider factory is unavailable; file capability is session-owned",
    ))
}

/// Heuristic used by the file tree to decide whether to offer "open in editor".
///
/// Conservative allow-list of common research/source/text extensions. The editor
/// additionally caps file size; binary detection happens on read.
pub fn looks_like_text(name: &str) -> bool {
    const TEXT_EXTS: &[&str] = &[
        "txt",
        "md",
        "rst",
        "log",
        "csv",
        "tsv",
        "json",
        "yaml",
        "yml",
        "toml",
        "ini",
        "cfg",
        "conf",
        "env",
        "py",
        "pyi",
        "ipynb",
        "c",
        "h",
        "cc",
        "cpp",
        "cxx",
        "hpp",
        "hxx",
        "rs",
        "go",
        "java",
        "kt",
        "js",
        "ts",
        "tsx",
        "jsx",
        "sh",
        "bash",
        "zsh",
        "fish",
        "r",
        "jl",
        "m",
        "lua",
        "pl",
        "rb",
        "php",
        "sql",
        "html",
        "css",
        "xml",
        "tex",
        "bib",
        "make",
        "cmake",
        "gradle",
        "dockerfile",
        "gitignore",
    ];
    // Files like "Makefile"/"Dockerfile"/".gitignore" with no extension.
    let lower = name.to_ascii_lowercase();
    if matches!(
        lower.as_str(),
        "makefile" | "dockerfile" | "cmakelists.txt" | ".gitignore"
    ) {
        return true;
    }
    match lower.rsplit_once('.') {
        Some((_, ext)) => TEXT_EXTS.contains(&ext),
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn standalone_file_provider_factory_is_unavailable() {
        let err = create_file_provider(RsProviderKind::Russh)
            .err()
            .expect("expected unavailable error");
        assert_eq!(err.code, RsErrorCode::ProviderUnavailable);
    }

    #[test]
    fn text_detection_allows_sources_rejects_binaries() {
        assert!(looks_like_text("train.py"));
        assert!(looks_like_text("kernel.cpp"));
        assert!(looks_like_text("notes.md"));
        assert!(looks_like_text("Makefile"));
        assert!(looks_like_text(".gitignore"));
        assert!(!looks_like_text("model.bin"));
        assert!(!looks_like_text("photo.png"));
        assert!(!looks_like_text("archive"));
    }
}
