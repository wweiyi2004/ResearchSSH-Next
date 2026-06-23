//! The stable C ABI boundary.
//!
//! This is the ONLY module permitted to use `unsafe` (see the crate-level
//! `#![deny(unsafe_code)]` plus `#[allow(unsafe_code)]` on this module). Every
//! `unsafe` block here documents the contract it relies on.
//!
//! # Ownership & safety contract (mirrored in README + docs/architecture.md)
//! * Handles are opaque, heap-allocated and owned by Rust. The C++ side stores
//!   the pointer and must call the matching `*_destroy` exactly once. Using a
//!   handle after destroying it is undefined behaviour.
//! * No Rust object is ever exposed by value; nothing but POD `#[repr(C)]`
//!   structs and primitive types cross the boundary.
//! * Every entry point is wrapped in `catch_unwind`; a panic never unwinds across
//!   the boundary (it becomes `RsErrorCode::Panic` or a null pointer).
//! * Pointers inside [`RsEvent`] (`data`, `message`) are owned by Rust and are
//!   valid ONLY for the duration of the callback. The C++ side must copy them
//!   before returning.
//! * The event callback may be invoked from arbitrary Rust runtime threads. The
//!   C++ side is responsible for marshalling onto the Qt UI thread
//!   (`Qt::QueuedConnection`). After `rscore_session_destroy` returns, the driver
//!   task has been joined, so no further callbacks can fire.

// These are C ABI entry points called from C/C++, which has no notion of Rust's
// `unsafe fn`. Their preconditions (valid, live handles; valid pointers) are part
// of the ABI contract documented in the module header and the generated C header,
// and each raw-pointer dereference is justified with a local SAFETY comment.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use crate::event::{RsEvent, RsEventKind, RsSessionState, SessionEvent};
use crate::fs::{FileKind, FilePayload, FileResult};
use crate::provider::ConnectionConfig;
use crate::runtime::CoreRuntime;
use crate::secret::Secret;
use crate::session::{EventEmitter, FileEmitter, Session};
use crate::{CoreResult, RsErrorCode};
use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::Arc;

// ---------------------------------------------------------------------------
// Opaque handles (cbindgen emits forward-declared opaque structs for these).
// ---------------------------------------------------------------------------

/// Opaque handle to the core (owns the async runtime).
pub struct RsCore {
    runtime: Arc<CoreRuntime>,
}

/// Opaque handle to a single session.
pub struct RsSession {
    session: Session,
}

// ---------------------------------------------------------------------------
// Callback + config types crossing the boundary.
// ---------------------------------------------------------------------------

/// Event callback. Receives a batch of `count` events. See the module-level
/// ownership contract for pointer lifetimes and threading rules.
pub type RsEventCallback =
    extern "C" fn(user_data: *mut c_void, events: *const RsEvent, count: usize);

/// Parameters for creating a session. Strings are borrowed, NUL-terminated UTF-8;
/// they are copied during the call and need not outlive it.
#[repr(C)]
pub struct RsSessionConfig {
    /// Hostname or IP (UTF-8, NUL-terminated).
    pub host: *const c_char,
    /// TCP port.
    pub port: u16,
    /// Login user name (UTF-8, NUL-terminated).
    pub username: *const c_char,
    /// Which provider to use.
    pub provider: crate::provider::RsProviderKind,
}

// ---------------------------------------------------------------------------
// FFI event emitter: translates owned events to RsEvent and invokes the callback
// exactly once per batch.
// ---------------------------------------------------------------------------

struct CallbackTarget {
    cb: RsEventCallback,
    user_data: *mut c_void,
}

// SAFETY: `user_data` is an opaque pointer owned by the C++ side, which guarantees
// it remains valid for the lifetime of the session and is safe to touch from other
// threads (it marshals onto the Qt UI thread via a queued connection). The driver
// is joined in `rscore_session_destroy` before the C++ owner frees `user_data`, so
// the callback is never invoked with a dangling pointer.
unsafe impl Send for CallbackTarget {}
unsafe impl Sync for CallbackTarget {}

struct FfiEmitter {
    target: CallbackTarget,
}

impl EventEmitter for FfiEmitter {
    fn emit_batch(&self, events: Vec<SessionEvent>) {
        if events.is_empty() {
            return;
        }

        // Move all payloads into stable backing storage FIRST. The heap buffers
        // behind each `Vec<u8>` / `CString` keep their address even if the outer
        // `Vec` reallocates, so pointers taken afterwards stay valid.
        enum Record {
            State(RsSessionState),
            Data(usize),
            Error(RsErrorCode, usize),
            HostKey(usize),
        }
        let mut data_backing: Vec<Vec<u8>> = Vec::new();
        let mut msg_backing: Vec<CString> = Vec::new();
        let mut records: Vec<Record> = Vec::with_capacity(events.len());

        // CString rejects interior NULs; sanitise defensively.
        let to_cstring = |s: String| CString::new(s.replace('\0', "?")).unwrap_or_default();

        for ev in events {
            match ev {
                SessionEvent::StateChanged(s) => records.push(Record::State(s)),
                SessionEvent::Data(bytes) => {
                    data_backing.push(bytes);
                    records.push(Record::Data(data_backing.len() - 1));
                }
                SessionEvent::Error { code, message } => {
                    msg_backing.push(to_cstring(message));
                    records.push(Record::Error(code, msg_backing.len() - 1));
                }
                SessionEvent::HostKeyPrompt { fingerprint } => {
                    msg_backing.push(to_cstring(fingerprint));
                    records.push(Record::HostKey(msg_backing.len() - 1));
                }
            }
        }

        let mut c_events: Vec<RsEvent> = Vec::with_capacity(records.len());
        for r in &records {
            let ev = match *r {
                Record::State(s) => RsEvent {
                    kind: RsEventKind::StateChanged,
                    state: s,
                    error_code: RsErrorCode::Ok,
                    data: ptr::null(),
                    data_len: 0,
                    message: ptr::null(),
                },
                Record::Data(i) => RsEvent {
                    kind: RsEventKind::Data,
                    state: RsSessionState::Idle,
                    error_code: RsErrorCode::Ok,
                    data: data_backing[i].as_ptr(),
                    data_len: data_backing[i].len(),
                    message: ptr::null(),
                },
                Record::Error(code, i) => RsEvent {
                    kind: RsEventKind::Error,
                    state: RsSessionState::Idle,
                    error_code: code,
                    data: ptr::null(),
                    data_len: 0,
                    message: msg_backing[i].as_ptr(),
                },
                Record::HostKey(i) => RsEvent {
                    kind: RsEventKind::HostKeyPrompt,
                    state: RsSessionState::Idle,
                    error_code: RsErrorCode::Ok,
                    data: ptr::null(),
                    data_len: 0,
                    message: msg_backing[i].as_ptr(),
                },
            };
            c_events.push(ev);
        }

        // `cb` is a safe `extern "C" fn` pointer (validated non-null at session
        // creation), so calling it needs no `unsafe`. The event/data/message
        // pointers are valid for the duration of this synchronous call; the backing
        // buffers are dropped only after the callback returns.
        (self.target.cb)(self.target.user_data, c_events.as_ptr(), c_events.len());
    }
}

// ---------------------------------------------------------------------------
// Panic guards.
// ---------------------------------------------------------------------------

fn guard_code<F: FnOnce() -> RsErrorCode>(f: F) -> RsErrorCode {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(RsErrorCode::Panic)
}

fn guard_ptr<T, F: FnOnce() -> *mut T>(f: F) -> *mut T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(ptr::null_mut())
}

fn guard_unit<F: FnOnce()>(f: F) {
    let _ = catch_unwind(AssertUnwindSafe(f));
}

// ---------------------------------------------------------------------------
// Small helpers (all unsafe pointer handling lives here).
// ---------------------------------------------------------------------------

/// Write `code` to `out` if `out` is non-null.
fn set_err(out: *mut RsErrorCode, code: RsErrorCode) {
    if !out.is_null() {
        // SAFETY: caller-provided out-param; we only write when non-null.
        unsafe { *out = code };
    }
}

/// Convert a borrowed C string to an owned `String`.
fn cstr_to_string(p: *const c_char) -> Result<String, RsErrorCode> {
    if p.is_null() {
        return Err(RsErrorCode::NullArgument);
    }
    // SAFETY: `p` is non-null and the ABI requires it to be a valid, NUL-terminated
    // C string for the duration of the call.
    let cstr = unsafe { CStr::from_ptr(p) };
    cstr.to_str()
        .map(|s| s.to_owned())
        .map_err(|_| RsErrorCode::InvalidUtf8)
}

/// Copy a borrowed byte buffer into an owned `Vec<u8>`. `len == 0` yields empty.
fn bytes_to_vec(data: *const u8, len: usize) -> Result<Vec<u8>, RsErrorCode> {
    if len == 0 {
        return Ok(Vec::new());
    }
    if data.is_null() {
        return Err(RsErrorCode::NullArgument);
    }
    // SAFETY: caller guarantees `data` points to at least `len` valid bytes.
    Ok(unsafe { std::slice::from_raw_parts(data, len) }.to_vec())
}

fn with_session<F>(session: *mut RsSession, f: F) -> RsErrorCode
where
    F: FnOnce(&Session) -> CoreResult<()>,
{
    // SAFETY: `session` must be a live handle returned by `rscore_session_create`.
    match unsafe { session.as_ref() } {
        Some(s) => match f(&s.session) {
            Ok(()) => RsErrorCode::Ok,
            Err(e) => e.code,
        },
        None => RsErrorCode::NullArgument,
    }
}

// ---------------------------------------------------------------------------
// Public C ABI.
// ---------------------------------------------------------------------------

/// Returns the core version as a static, NUL-terminated UTF-8 string. The pointer
/// is valid for the lifetime of the process and must NOT be freed.
#[no_mangle]
pub extern "C" fn rscore_version() -> *const c_char {
    const VERSION: &CStr =
        match CStr::from_bytes_with_nul(concat!(env!("CARGO_PKG_VERSION"), "\0").as_bytes()) {
            Ok(s) => s,
            Err(_) => c"unknown",
        };
    VERSION.as_ptr()
}

/// Returns a static, NUL-terminated UTF-8 description of an error code. Must NOT
/// be freed.
#[no_mangle]
pub extern "C" fn rscore_error_message(code: RsErrorCode) -> *const c_char {
    let s: &CStr = match code {
        RsErrorCode::Ok => c"ok",
        RsErrorCode::NullArgument => c"a required pointer argument was null",
        RsErrorCode::InvalidUtf8 => c"argument was not valid UTF-8",
        RsErrorCode::InvalidState => c"operation invalid in current session state",
        RsErrorCode::ProviderUnavailable => c"requested provider or capability is unavailable",
        RsErrorCode::ConnectFailed => c"failed to establish the connection",
        RsErrorCode::NotConnected => c"session is not connected",
        RsErrorCode::Cancelled => c"operation was cancelled",
        RsErrorCode::RuntimeError => c"async runtime error",
        RsErrorCode::Panic => c"a panic was caught at the FFI boundary",
        RsErrorCode::Internal => c"internal error",
    };
    s.as_ptr()
}

/// Frees a heap string previously returned by an API that transfers ownership.
/// Safe to call with null. (Reserved: current APIs return only static strings.)
#[no_mangle]
pub extern "C" fn rscore_string_free(s: *mut c_char) {
    guard_unit(|| {
        if !s.is_null() {
            // SAFETY: `s` must have been produced by `CString::into_raw` in this
            // library; ownership is taken back and dropped here.
            unsafe { drop(CString::from_raw(s)) };
        }
    });
}

/// Creates the core (and its async runtime). On failure returns null and writes a
/// code to `out_err` (if non-null). Destroy with [`rscore_destroy`].
#[no_mangle]
pub extern "C" fn rscore_create(out_err: *mut RsErrorCode) -> *mut RsCore {
    guard_ptr(|| match CoreRuntime::new() {
        Ok(runtime) => {
            set_err(out_err, RsErrorCode::Ok);
            Box::into_raw(Box::new(RsCore { runtime }))
        }
        Err(e) => {
            set_err(out_err, e.code);
            ptr::null_mut()
        }
    })
}

/// Destroys a core created by [`rscore_create`]. Safe to call with null. All
/// sessions created from this core must be destroyed first.
#[no_mangle]
pub extern "C" fn rscore_destroy(core: *mut RsCore) {
    guard_unit(|| {
        if !core.is_null() {
            // SAFETY: `core` was produced by `Box::into_raw` in `rscore_create`
            // and is destroyed exactly once.
            unsafe { drop(Box::from_raw(core)) };
        }
    });
}

/// Creates a session on `core`. Returns null on failure and writes a code to
/// `out_err`. The `cb`/`user_data` pair receives batched events. Destroy with
/// [`rscore_session_destroy`].
#[no_mangle]
pub extern "C" fn rscore_session_create(
    core: *mut RsCore,
    config: *const RsSessionConfig,
    // The function pointer type is spelled out inline (rather than via the
    // `RsEventCallback` alias) so cbindgen emits a proper nullable C function
    // pointer for this parameter instead of an opaque type.
    cb: Option<extern "C" fn(user_data: *mut c_void, events: *const RsEvent, count: usize)>,
    user_data: *mut c_void,
    out_err: *mut RsErrorCode,
) -> *mut RsSession {
    guard_ptr(|| {
        // SAFETY: `core` must be a live handle.
        let core = match unsafe { core.as_ref() } {
            Some(c) => c,
            None => {
                set_err(out_err, RsErrorCode::NullArgument);
                return ptr::null_mut();
            }
        };
        // SAFETY: `config` must point to a valid RsSessionConfig.
        let config = match unsafe { config.as_ref() } {
            Some(c) => c,
            None => {
                set_err(out_err, RsErrorCode::NullArgument);
                return ptr::null_mut();
            }
        };
        let cb = match cb {
            Some(cb) => cb,
            None => {
                set_err(out_err, RsErrorCode::NullArgument);
                return ptr::null_mut();
            }
        };

        let host = match cstr_to_string(config.host) {
            Ok(s) => s,
            Err(code) => {
                set_err(out_err, code);
                return ptr::null_mut();
            }
        };
        let username = match cstr_to_string(config.username) {
            Ok(s) => s,
            Err(code) => {
                set_err(out_err, code);
                return ptr::null_mut();
            }
        };

        let conn = ConnectionConfig {
            host,
            port: config.port,
            username,
            kind: config.provider,
        };
        let emitter: Arc<dyn EventEmitter> = Arc::new(FfiEmitter {
            target: CallbackTarget { cb, user_data },
        });
        let session = Session::spawn(core.runtime.handle(), conn, emitter);
        set_err(out_err, RsErrorCode::Ok);
        Box::into_raw(Box::new(RsSession { session }))
    })
}

/// Destroys a session. Blocks briefly to join the driver task so no further
/// callbacks can fire after this returns. Safe to call with null.
#[no_mangle]
pub extern "C" fn rscore_session_destroy(session: *mut RsSession) {
    guard_unit(|| {
        if session.is_null() {
            return;
        }
        // SAFETY: `session` was produced by `Box::into_raw` in
        // `rscore_session_create` and is destroyed exactly once.
        let boxed = unsafe { Box::from_raw(session) };
        boxed.session.shutdown_and_join();
        drop(boxed);
    });
}

/// Requests a connection. Returns [`RsErrorCode::Ok`] if the request was queued.
#[no_mangle]
pub extern "C" fn rscore_session_connect(session: *mut RsSession) -> RsErrorCode {
    guard_code(|| with_session(session, |s| s.connect()))
}

/// Requests a disconnect.
#[no_mangle]
pub extern "C" fn rscore_session_disconnect(session: *mut RsSession) -> RsErrorCode {
    guard_code(|| with_session(session, |s| s.disconnect()))
}

/// Sends `len` bytes (a command line / keystrokes) to the remote.
#[no_mangle]
pub extern "C" fn rscore_session_send(
    session: *mut RsSession,
    data: *const u8,
    len: usize,
) -> RsErrorCode {
    guard_code(|| {
        let bytes = match bytes_to_vec(data, len) {
            Ok(b) => b,
            Err(code) => return code,
        };
        with_session(session, |s| s.send(bytes))
    })
}

/// Cancels the in-flight operation (if any). Always succeeds for a live handle.
#[no_mangle]
pub extern "C" fn rscore_session_cancel(session: *mut RsSession) -> RsErrorCode {
    guard_code(|| {
        // SAFETY: `session` must be a live handle.
        match unsafe { session.as_ref() } {
            Some(s) => {
                s.session.cancel();
                RsErrorCode::Ok
            }
            None => RsErrorCode::NullArgument,
        }
    })
}

/// Delivers the user's decision for a pending host-key prompt (the
/// `RsEventKind_HostKeyPrompt` event). `accept = true` trusts the key and lets the
/// connection proceed; `false` rejects it and aborts the connection. No-op if no
/// prompt is pending.
#[no_mangle]
pub extern "C" fn rscore_session_confirm_host_key(
    session: *mut RsSession,
    accept: bool,
) -> RsErrorCode {
    guard_code(|| {
        // SAFETY: `session` must be a live handle.
        match unsafe { session.as_ref() } {
            Some(s) => {
                s.session.confirm_host_key(accept);
                RsErrorCode::Ok
            }
            None => RsErrorCode::NullArgument,
        }
    })
}

/// Hands a sensitive credential (e.g. password bytes) to the session's provider.
/// The bytes are copied into a zeroizing buffer immediately. Reserved for real
/// providers; the mock discards it.
#[no_mangle]
pub extern "C" fn rscore_session_set_password(
    session: *mut RsSession,
    data: *const u8,
    len: usize,
) -> RsErrorCode {
    guard_code(|| {
        let bytes = match bytes_to_vec(data, len) {
            Ok(b) => b,
            Err(code) => return code,
        };
        let secret = Secret::from_bytes(&bytes);
        with_session(session, |s| s.set_secret(secret))
    })
}

// ---------------------------------------------------------------------------
// File management (SFTP) C ABI.
//
// Each `rscore_session_fs_*` call returns a non-zero request id (0 = could not
// queue). The matching `RsFsResult` (same `request_id`) is delivered later via the
// file callback registered with `rscore_session_set_file_callback`. Threading and
// pointer-ownership rules are identical to the event callback: the callback runs on
// a Rust thread; `entries`/`data`/`message`/`RsFileEntry.name`/`.path` are valid
// only for the duration of the call — copy them out.
// ---------------------------------------------------------------------------

/// Kind of a remote directory entry.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub enum RsFileKind {
    /// Regular file.
    File = 0,
    /// Directory.
    Directory = 1,
    /// Symbolic link.
    Symlink = 2,
    /// Anything else.
    Other = 3,
}

/// C-ABI view of one directory entry. Pointers valid only during the callback.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct RsFileEntry {
    /// Base name (UTF-8).
    pub name: *const c_char,
    /// Full remote path (UTF-8).
    pub path: *const c_char,
    /// Entry kind.
    pub kind: RsFileKind,
    /// Size in bytes.
    pub size: u64,
    /// Last-modified time (Unix seconds), -1 if unknown.
    pub modified_unix: i64,
    /// POSIX permission bits.
    pub mode: u32,
    /// Whether the in-app editor should offer to open this as text.
    pub editable_text: bool,
}

/// Discriminator for [`RsFsResult`].
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RsFsResultKind {
    /// `entries`/`entry_count` are valid (directory listing or single stat).
    Listing = 0,
    /// `data`/`data_len` are valid (file contents).
    Content = 1,
    /// A mutating op succeeded; no payload.
    Ok = 2,
    /// `error_code`/`message` are valid.
    Error = 3,
}

/// Result of a file operation, correlated by `request_id`.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct RsFsResult {
    /// Matches the id returned by the `rscore_session_fs_*` call.
    pub request_id: u64,
    /// Which fields are valid.
    pub kind: RsFsResultKind,
    /// Listing entries (valid when `kind == Listing`).
    pub entries: *const RsFileEntry,
    /// Number of entries.
    pub entry_count: usize,
    /// File bytes (valid when `kind == Content`).
    pub data: *const u8,
    /// Length of `data`.
    pub data_len: usize,
    /// Error code (valid when `kind == Error`).
    pub error_code: RsErrorCode,
    /// UTF-8 error message (valid when `kind == Error`).
    pub message: *const c_char,
}

/// File-result callback: receives a batch of `count` results.
pub type RsFileCallback =
    extern "C" fn(user_data: *mut c_void, results: *const RsFsResult, count: usize);

struct FileCallbackTarget {
    cb: RsFileCallback,
    user_data: *mut c_void,
}
// SAFETY: same contract as `CallbackTarget` — the C++ owner keeps `user_data` valid
// for the session's lifetime and is responsible for cross-thread access.
unsafe impl Send for FileCallbackTarget {}
unsafe impl Sync for FileCallbackTarget {}

struct FfiFileEmitter {
    target: FileCallbackTarget,
}

fn map_kind(kind: FileKind) -> RsFileKind {
    match kind {
        FileKind::File => RsFileKind::File,
        FileKind::Directory => RsFileKind::Directory,
        FileKind::Symlink => RsFileKind::Symlink,
        FileKind::Other => RsFileKind::Other,
    }
}

impl FileEmitter for FfiFileEmitter {
    fn emit_file_results(&self, results: Vec<FileResult>) {
        if results.is_empty() {
            return;
        }

        // Owned, stable backing storage. Heap buffers behind each CString / Vec keep
        // their address across outer Vec reallocations, so pointers taken after all
        // pushes are done remain valid for the callback.
        let to_cstring = |s: String| CString::new(s.replace('\0', "?")).unwrap_or_default();
        let mut cstrings: Vec<CString> = Vec::new();
        let mut byte_bufs: Vec<Vec<u8>> = Vec::new();

        // Intermediate description, resolved to pointers after all strings exist.
        struct EntryIdx {
            name: usize,
            path: usize,
            kind: RsFileKind,
            size: u64,
            modified_unix: i64,
            mode: u32,
            editable_text: bool,
        }
        enum Rec {
            Listing(Vec<EntryIdx>),
            Content(usize),
            Ok,
            Error(RsErrorCode, usize),
        }
        let mut recs: Vec<(u64, Rec)> = Vec::with_capacity(results.len());

        for r in results {
            let rec = match r.payload {
                FilePayload::Listing(entries) => {
                    let mut idxs = Vec::with_capacity(entries.len());
                    for e in entries {
                        cstrings.push(to_cstring(e.name));
                        let name = cstrings.len() - 1;
                        cstrings.push(to_cstring(e.path));
                        let path = cstrings.len() - 1;
                        idxs.push(EntryIdx {
                            name,
                            path,
                            kind: map_kind(e.kind),
                            size: e.size,
                            modified_unix: e.modified_unix,
                            mode: e.mode,
                            editable_text: e.editable_text,
                        });
                    }
                    Rec::Listing(idxs)
                }
                FilePayload::Content(bytes) => {
                    byte_bufs.push(bytes);
                    Rec::Content(byte_bufs.len() - 1)
                }
                FilePayload::Ok => Rec::Ok,
                FilePayload::Error { code, message } => {
                    cstrings.push(to_cstring(message));
                    Rec::Error(code, cstrings.len() - 1)
                }
            };
            recs.push((r.request_id, rec));
        }

        // All strings now exist → build the entry arrays (stable storage).
        let mut entry_arrays: Vec<Vec<RsFileEntry>> = Vec::with_capacity(recs.len());
        for (_, rec) in &recs {
            let arr = match rec {
                Rec::Listing(idxs) => idxs
                    .iter()
                    .map(|e| RsFileEntry {
                        name: cstrings[e.name].as_ptr(),
                        path: cstrings[e.path].as_ptr(),
                        kind: e.kind,
                        size: e.size,
                        modified_unix: e.modified_unix,
                        mode: e.mode,
                        editable_text: e.editable_text,
                    })
                    .collect(),
                _ => Vec::new(),
            };
            entry_arrays.push(arr);
        }

        // Entry arrays now stable → build the result array.
        let mut c_results: Vec<RsFsResult> = Vec::with_capacity(recs.len());
        for (i, (request_id, rec)) in recs.iter().enumerate() {
            let result = match rec {
                Rec::Listing(_) => RsFsResult {
                    request_id: *request_id,
                    kind: RsFsResultKind::Listing,
                    entries: entry_arrays[i].as_ptr(),
                    entry_count: entry_arrays[i].len(),
                    data: ptr::null(),
                    data_len: 0,
                    error_code: RsErrorCode::Ok,
                    message: ptr::null(),
                },
                Rec::Content(bi) => RsFsResult {
                    request_id: *request_id,
                    kind: RsFsResultKind::Content,
                    entries: ptr::null(),
                    entry_count: 0,
                    data: byte_bufs[*bi].as_ptr(),
                    data_len: byte_bufs[*bi].len(),
                    error_code: RsErrorCode::Ok,
                    message: ptr::null(),
                },
                Rec::Ok => RsFsResult {
                    request_id: *request_id,
                    kind: RsFsResultKind::Ok,
                    entries: ptr::null(),
                    entry_count: 0,
                    data: ptr::null(),
                    data_len: 0,
                    error_code: RsErrorCode::Ok,
                    message: ptr::null(),
                },
                Rec::Error(code, mi) => RsFsResult {
                    request_id: *request_id,
                    kind: RsFsResultKind::Error,
                    entries: ptr::null(),
                    entry_count: 0,
                    data: ptr::null(),
                    data_len: 0,
                    error_code: *code,
                    message: cstrings[*mi].as_ptr(),
                },
            };
            c_results.push(result);
        }

        (self.target.cb)(self.target.user_data, c_results.as_ptr(), c_results.len());
    }
}

fn guard_u64<F: FnOnce() -> u64>(f: F) -> u64 {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(0)
}

/// Registers the file-result callback for a session. Required before any
/// `rscore_session_fs_*` call delivers results.
#[no_mangle]
pub extern "C" fn rscore_session_set_file_callback(
    session: *mut RsSession,
    cb: Option<extern "C" fn(user_data: *mut c_void, results: *const RsFsResult, count: usize)>,
    user_data: *mut c_void,
) -> RsErrorCode {
    guard_code(|| {
        let cb = match cb {
            Some(cb) => cb,
            None => return RsErrorCode::NullArgument,
        };
        // SAFETY: `session` must be a live handle.
        match unsafe { session.as_ref() } {
            Some(s) => {
                let emitter: Arc<dyn FileEmitter> = Arc::new(FfiFileEmitter {
                    target: FileCallbackTarget { cb, user_data },
                });
                s.session.set_file_emitter(emitter);
                RsErrorCode::Ok
            }
            None => RsErrorCode::NullArgument,
        }
    })
}

fn fs_with_session<F: FnOnce(&Session) -> u64>(session: *mut RsSession, f: F) -> u64 {
    // SAFETY: `session` must be a live handle.
    match unsafe { session.as_ref() } {
        Some(s) => f(&s.session),
        None => 0,
    }
}

/// Lists a directory (empty `path` = the connection's home dir). Returns a request id.
#[no_mangle]
pub extern "C" fn rscore_session_fs_list(session: *mut RsSession, path: *const c_char) -> u64 {
    guard_u64(|| match cstr_to_string(path) {
        Ok(p) => fs_with_session(session, |s| s.fs_list(p)),
        Err(_) => 0,
    })
}

/// Stats a single path (result delivered as a one-element listing).
#[no_mangle]
pub extern "C" fn rscore_session_fs_stat(session: *mut RsSession, path: *const c_char) -> u64 {
    guard_u64(|| match cstr_to_string(path) {
        Ok(p) => fs_with_session(session, |s| s.fs_stat(p)),
        Err(_) => 0,
    })
}

/// Reads up to `max_len` bytes of a file. Returns a request id.
#[no_mangle]
pub extern "C" fn rscore_session_fs_read(
    session: *mut RsSession,
    path: *const c_char,
    max_len: u64,
) -> u64 {
    guard_u64(|| match cstr_to_string(path) {
        Ok(p) => fs_with_session(session, |s| s.fs_read(p, max_len)),
        Err(_) => 0,
    })
}

/// Overwrites a file with `len` bytes (editor save / upload). Returns a request id.
#[no_mangle]
pub extern "C" fn rscore_session_fs_write(
    session: *mut RsSession,
    path: *const c_char,
    data: *const u8,
    len: usize,
) -> u64 {
    guard_u64(|| {
        let path = match cstr_to_string(path) {
            Ok(p) => p,
            Err(_) => return 0,
        };
        let bytes = match bytes_to_vec(data, len) {
            Ok(b) => b,
            Err(_) => return 0,
        };
        fs_with_session(session, |s| s.fs_write(path, bytes))
    })
}

/// Renames / moves `from` to `to`. Returns a request id.
#[no_mangle]
pub extern "C" fn rscore_session_fs_rename(
    session: *mut RsSession,
    from: *const c_char,
    to: *const c_char,
) -> u64 {
    guard_u64(|| {
        let from = match cstr_to_string(from) {
            Ok(p) => p,
            Err(_) => return 0,
        };
        let to = match cstr_to_string(to) {
            Ok(p) => p,
            Err(_) => return 0,
        };
        fs_with_session(session, |s| s.fs_rename(from, to))
    })
}

/// Removes a path (recursively for directories when `recursive`). Returns a request id.
#[no_mangle]
pub extern "C" fn rscore_session_fs_remove(
    session: *mut RsSession,
    path: *const c_char,
    recursive: bool,
) -> u64 {
    guard_u64(|| match cstr_to_string(path) {
        Ok(p) => fs_with_session(session, |s| s.fs_remove(p, recursive)),
        Err(_) => 0,
    })
}

/// Creates a directory. Returns a request id.
#[no_mangle]
pub extern "C" fn rscore_session_fs_mkdir(session: *mut RsSession, path: *const c_char) -> u64 {
    guard_u64(|| match cstr_to_string(path) {
        Ok(p) => fs_with_session(session, |s| s.fs_mkdir(p)),
        Err(_) => 0,
    })
}

/// Copies `from` to `to`. Returns a request id.
#[no_mangle]
pub extern "C" fn rscore_session_fs_copy(
    session: *mut RsSession,
    from: *const c_char,
    to: *const c_char,
) -> u64 {
    guard_u64(|| {
        let from = match cstr_to_string(from) {
            Ok(p) => p,
            Err(_) => return 0,
        };
        let to = match cstr_to_string(to) {
            Ok(p) => p,
            Err(_) => return 0,
        };
        fs_with_session(session, |s| s.fs_copy(from, to))
    })
}

// ---------------------------------------------------------------------------
// FFI smoke test: exercises the real C entry points from Rust.
// ---------------------------------------------------------------------------
#[cfg(test)]
mod tests {
    use super::*;
    use crate::provider::RsProviderKind;
    use std::sync::Mutex;
    use std::time::Duration;

    #[derive(Default)]
    struct Collector {
        states: Mutex<Vec<RsSessionState>>,
        data: Mutex<Vec<u8>>,
        errors: Mutex<Vec<RsErrorCode>>,
    }

    extern "C" fn collect_cb(user_data: *mut c_void, events: *const RsEvent, count: usize) {
        // SAFETY: `user_data` is a `&Collector` we passed in; events/count describe
        // a valid slice for the duration of this call.
        let collector = unsafe { &*(user_data as *const Collector) };
        let evs = unsafe { std::slice::from_raw_parts(events, count) };
        for ev in evs {
            match ev.kind {
                RsEventKind::StateChanged => collector.states.lock().unwrap().push(ev.state),
                RsEventKind::Data => {
                    let d = unsafe { std::slice::from_raw_parts(ev.data, ev.data_len) };
                    collector.data.lock().unwrap().extend_from_slice(d);
                }
                RsEventKind::Error => collector.errors.lock().unwrap().push(ev.error_code),
                RsEventKind::HostKeyPrompt => {}
            }
        }
    }

    #[test]
    fn version_and_error_message_are_valid() {
        let v = unsafe { CStr::from_ptr(rscore_version()) }
            .to_str()
            .unwrap();
        assert!(v.contains('.'), "version looks like semver: {v}");
        let m = unsafe { CStr::from_ptr(rscore_error_message(RsErrorCode::Ok)) }
            .to_str()
            .unwrap();
        assert_eq!(m, "ok");
    }

    #[test]
    fn ffi_smoke_full_session_lifecycle() {
        let mut err = RsErrorCode::Internal;
        let core = rscore_create(&mut err);
        assert!(!core.is_null());
        assert_eq!(err, RsErrorCode::Ok);

        let collector = Collector::default();
        let host = CString::new("hpc.example.edu").unwrap();
        let user = CString::new("researcher").unwrap();
        let config = RsSessionConfig {
            host: host.as_ptr(),
            port: 22,
            username: user.as_ptr(),
            provider: RsProviderKind::Mock,
        };

        let session = rscore_session_create(
            core,
            &config,
            Some(collect_cb),
            &collector as *const Collector as *mut c_void,
            &mut err,
        );
        assert!(!session.is_null());
        assert_eq!(err, RsErrorCode::Ok);

        assert_eq!(rscore_session_connect(session), RsErrorCode::Ok);
        std::thread::sleep(Duration::from_millis(450));

        let cmd = b"squeue\n";
        assert_eq!(
            rscore_session_send(session, cmd.as_ptr(), cmd.len()),
            RsErrorCode::Ok
        );
        std::thread::sleep(Duration::from_millis(300));

        assert_eq!(rscore_session_disconnect(session), RsErrorCode::Ok);
        std::thread::sleep(Duration::from_millis(250));

        // Destroy joins the driver; no callbacks fire after this returns.
        rscore_session_destroy(session);
        rscore_destroy(core);

        let states = collector.states.lock().unwrap();
        assert!(states.contains(&RsSessionState::Connected));
        assert!(states.contains(&RsSessionState::Disconnected));
        assert!(collector.errors.lock().unwrap().is_empty());
        let data_guard = collector.data.lock().unwrap();
        let text = String::from_utf8_lossy(&data_guard);
        assert!(text.contains("hpc.example.edu"));
        assert!(text.contains("squeue"));
        assert!(text.contains("JOBID"));
    }

    #[test]
    fn null_handles_are_rejected_not_crashed() {
        assert_eq!(
            rscore_session_connect(ptr::null_mut()),
            RsErrorCode::NullArgument
        );
        rscore_session_destroy(ptr::null_mut()); // must not crash
        rscore_destroy(ptr::null_mut()); // must not crash
    }

    // Full file-management pipeline over the C ABI, against the in-memory mock file
    // provider (no SFTP server needed). Exercises fs_list/read/write/mkdir + the
    // request-id-correlated file callback.
    #[derive(Default)]
    struct FsCollector {
        results: Mutex<Vec<(u64, String)>>,
    }

    extern "C" fn noop_events(_u: *mut c_void, _e: *const RsEvent, _c: usize) {}

    extern "C" fn fs_collect(user_data: *mut c_void, results: *const RsFsResult, count: usize) {
        let c = unsafe { &*(user_data as *const FsCollector) };
        let rs = unsafe { std::slice::from_raw_parts(results, count) };
        for r in rs {
            let summary = match r.kind {
                RsFsResultKind::Listing => {
                    let entries = unsafe { std::slice::from_raw_parts(r.entries, r.entry_count) };
                    let names: Vec<String> = entries
                        .iter()
                        .map(|e| {
                            unsafe { CStr::from_ptr(e.name) }
                                .to_string_lossy()
                                .into_owned()
                        })
                        .collect();
                    format!("listing:{}", names.join(","))
                }
                RsFsResultKind::Content => {
                    let d = unsafe { std::slice::from_raw_parts(r.data, r.data_len) };
                    format!("content:{}", String::from_utf8_lossy(d))
                }
                RsFsResultKind::Ok => "ok".to_string(),
                RsFsResultKind::Error => "error".to_string(),
            };
            c.results.lock().unwrap().push((r.request_id, summary));
        }
    }

    #[test]
    fn ffi_file_management_pipeline_over_mock() {
        let mut err = RsErrorCode::Internal;
        let core = rscore_create(&mut err);
        assert!(!core.is_null());

        let collector = FsCollector::default();
        let host = CString::new("hpc.example.edu").unwrap();
        let user = CString::new("researcher").unwrap();
        let config = RsSessionConfig {
            host: host.as_ptr(),
            port: 22,
            username: user.as_ptr(),
            provider: RsProviderKind::Mock,
        };
        let mut e2 = RsErrorCode::Internal;
        let session =
            rscore_session_create(core, &config, Some(noop_events), ptr::null_mut(), &mut e2);
        assert!(!session.is_null());

        assert_eq!(
            rscore_session_set_file_callback(
                session,
                Some(fs_collect),
                &collector as *const FsCollector as *mut c_void,
            ),
            RsErrorCode::Ok
        );
        assert_eq!(rscore_session_connect(session), RsErrorCode::Ok);

        // Commands are processed in order after connect completes.
        let empty = CString::new("").unwrap();
        let id_list = rscore_session_fs_list(session, empty.as_ptr());
        let read_path = CString::new("/home/researcher/train.py").unwrap();
        let id_read = rscore_session_fs_read(session, read_path.as_ptr(), 0);
        let new_dir = CString::new("/home/researcher/newdir").unwrap();
        let id_mkdir = rscore_session_fs_mkdir(session, new_dir.as_ptr());
        let note = CString::new("/home/researcher/note.txt").unwrap();
        let payload = b"hello from the editor";
        let id_write =
            rscore_session_fs_write(session, note.as_ptr(), payload.as_ptr(), payload.len());
        let id_list2 = rscore_session_fs_list(session, empty.as_ptr());

        assert!(id_list != 0 && id_read != 0 && id_mkdir != 0 && id_write != 0 && id_list2 != 0);

        std::thread::sleep(Duration::from_millis(800));
        rscore_session_destroy(session);
        rscore_destroy(core);

        let results = collector.results.lock().unwrap();
        let find = |id: u64| -> String {
            results
                .iter()
                .find(|(rid, _)| *rid == id)
                .map(|(_, s)| s.clone())
                .unwrap_or_default()
        };

        assert!(
            find(id_list).contains("train.py"),
            "list: {}",
            find(id_list)
        );
        assert!(find(id_read).contains("torch"), "read: {}", find(id_read));
        assert_eq!(find(id_mkdir), "ok");
        assert_eq!(find(id_write), "ok");
        let list2 = find(id_list2);
        assert!(list2.contains("note.txt"), "list2: {list2}");
        assert!(list2.contains("newdir"), "list2: {list2}");
    }
}
