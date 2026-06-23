# Remote File Management & In-App Editor

Status: **implemented baseline**. The file workflow is wired through Rust
`FileProvider`, the C ABI, `RustCoreBridge`, `RemoteFileTreeModel`,
`EditorViewModel`, `AppController`, and the QML **文件** tab. The Mock provider
ships an in-memory remote filesystem for demos and tests. With `--features russh`,
the real backend attempts to open an SFTP subsystem over `russh-sftp`; if the
server does not expose SFTP, the shell still works and file operations report
unavailable.

Goal features:

* a **file tree** that browses the server's folders (lazy-loaded per directory);
* **right-click actions**: copy / paste / rename / delete / new folder;
* **upload** a local file to the server;
* **open a text file** (`.py`, `.cpp`, …) in an **in-app editor pane**, edit, and
  **save back** to the server via SFTP.

> The "edit in a terminal `vim`/`nano`" alternative is intentionally *not* this
> design: it needs a real interactive PTY + a full VT/ANSI terminal emulator and
> is tracked separately. This design uses a GUI editor pane (WinSCP-style).

## Layering (unchanged)

```
QML: TreeView + context Menu + Editor pane   (no FFI, no secrets, no local FS access for secrets)
        │ properties / slots
C++: RemoteFileTreeModel (QAbstractItemModel, lazy)
     EditorViewModel (open/save text)
     file actions + request routing — owned by AppController
        │ stable C ABI (request id in, results via a file callback)
Rust: fs::FileProvider (SFTP ops)  ← runs on the session driver task
        │  (real backend) SFTP subsystem over russh-sftp
remote server
```

The file capability is a **second provider on the same session**: the session
keeps its `SshProvider` (shell) and gains an optional `Box<dyn FileProvider>`
(SFTP). Both are created from the same `ConnectionConfig`.

## Rust core

* `FileKind { File, Directory, Symlink, Other }`
* `FileEntry { name, path, kind, size, modified_unix, mode, editable_text }`
* `trait FileProvider`: `list_dir`, `stat`, `read_file(max_len)`, `write_file`,
  `rename`, `remove(recursive)`, `mkdir`, `copy`
* `MockFileProvider` — in-memory remote filesystem for the Mock backend.
* `SftpFileProvider` — `russh-sftp` client used by the `russh` backend when the
  server accepts an SFTP subsystem.
* `create_file_provider(kind)` — still reserved; active providers currently hand
  file capability to the session via `SshProvider::take_file_provider()`.
* `looks_like_text(name)` — extension allow-list for the "open in editor" affordance.

The session driver owns an optional `Box<dyn FileProvider>`, accepts file commands
with monotonic request ids, and delivers results through a file result callback.
Bulk data (`read`/`write`) crosses as one buffer.

## C ABI

File results are asynchronous and must be correlated with their request, so each
op returns a **request id** (`u64`, `0` = failed to queue) and the result arrives
later on a dedicated **file callback** registered per session.

### New POD types

```c
typedef enum { RsFileKind_File=0, RsFileKind_Directory=1,
               RsFileKind_Symlink=2, RsFileKind_Other=3 } RsFileKind;

typedef struct {
    const char *name;        // UTF-8, valid only during the callback
    const char *path;        // UTF-8, valid only during the callback
    RsFileKind  kind;
    uint64_t    size;
    int64_t     modified_unix;   // -1 if unknown
    uint32_t    mode;
    bool        editable_text;
} RsFileEntry;

typedef enum { RsFsResult_Listing=0, RsFsResult_Content=1,
               RsFsResult_Ok=2, RsFsResult_Error=3 } RsFsResultKind;

typedef struct {
    uint64_t        request_id;     // correlates with the call that returned it
    RsFsResultKind  kind;
    const RsFileEntry *entries;     // Listing: array, valid only during callback
    uintptr_t          entry_count;
    const uint8_t  *data;           // Content: file bytes, valid only during callback
    uintptr_t       data_len;
    RsErrorCode     error_code;     // Error
    const char     *message;        // Error: UTF-8, valid only during callback
} RsFsResult;

// Batched, exactly like RsEventCallback.
typedef void (*RsFileCallback)(void *user_data, const RsFsResult *results, uintptr_t count);
```

### New functions

```c
// Register the file-result callback for a session (same threading rules as the
// event callback: invoked on Rust threads; marshal to the UI thread).
RsErrorCode rscore_session_set_file_callback(RsSession *s, RsFileCallback cb, void *user_data);

// Each returns a request id (0 = could not queue). The RsFsResult with the same
// request_id is delivered later via the file callback.
uint64_t rscore_session_fs_list  (RsSession *s, const char *path);
uint64_t rscore_session_fs_stat  (RsSession *s, const char *path);
uint64_t rscore_session_fs_read  (RsSession *s, const char *path, uint64_t max_len);
uint64_t rscore_session_fs_write (RsSession *s, const char *path, const uint8_t *data, uintptr_t len);
uint64_t rscore_session_fs_rename(RsSession *s, const char *from, const char *to);
uint64_t rscore_session_fs_remove(RsSession *s, const char *path, bool recursive);
uint64_t rscore_session_fs_mkdir (RsSession *s, const char *path);
uint64_t rscore_session_fs_copy  (RsSession *s, const char *from, const char *to);
```

### Ownership rules (extend the existing FFI contract)

* `RsFsResult.entries`, `.data`, `.message`, and each `RsFileEntry.name`/`.path`
  are owned by Rust and valid **only for the duration of the callback**. Copy them
  out before returning (just like `RsEvent`).
* Results are **batched** (the callback may receive several `RsFsResult` at once).
* `request_id` is monotonic per session; `0` is never a valid id.
* Uploads need no special FFI: the C++ side reads the local file (it owns local
  filesystem access) and calls `rscore_session_fs_write`. A chunked/streamed
  upload API can be added later for very large files.
* Secrets remain out of this surface entirely.

## C++ adapter (`app/cpp`)

* **`RemoteFileTreeModel : QAbstractItemModel`** — lazy hierarchical model.
  * Roles: `name`, `path`, `kind`, `size`, `modified`, `editable`.
  * A tree node holds its path + a `loaded` flag + children. `hasChildren` is true
    for directories; expanding an unloaded directory calls `rscore_session_fs_list`
    and records `request_id → node`. The `RsFsResult_Listing` for that id (marshalled
    to the UI thread) populates the node's children and emits the model signals.
  * Refresh = re-list a node; rename/delete/mkdir/paste re-list the affected parent.
* **`EditorViewModel : QObject`** — `path`, `fileName`, `isOpen`, `busy`, `saving`.
  * `open(path)` → `fs_read(path, cap)`; the `Content` result sets `text`
    via `contentLoaded(QString)`.
  * `save()` → `fs_write(path, text.toUtf8())`.
* **File actions in `AppController`** — actions + a one-slot path "clipboard":
  * copy → remember source path; paste → `fs_copy(src, destDir + "/" + name)`.
  * rename → `fs_rename`; delete → `fs_remove(recursive for dirs)`;
    new folder → `fs_mkdir`.
  * upload → QML `FileDialog` passes a local file URL → C++ reads bytes →
    `fs_write(remoteDir + name, bytes)`.
  * All ops route their `request_id` so the controller can report success/error to
    the terminal notice area and refresh the tree.
  * The controller tracks the root file capability state (`fileAvailable`,
    `fileStatusText`). If the shell connects but SFTP is unavailable, the file tab
    disables file actions while the terminal remains usable.
  * The first successful root listing caches the concrete remote home path, so
    root-level mkdir/upload/paste targets are written under the real home instead
    of depending on ambiguous relative paths.
* The single file callback (registered via `rscore_session_set_file_callback`) lives
  in `RustCoreBridge`, copies each `RsFsResult` into Qt types, and posts to
  `AppController` with `Qt::QueuedConnection` — identical to the event callback path.

## QML (`app/qml`)

* The right workspace pane has tabs: **连接** and **文件**.
* The **文件** tab hosts a `TreeView` bound to `RemoteFileTreeModel`. It is now a
  dedicated file explorer with Windows-sidebar-style rows, compact folder/file
  icons, selected path display, and full-path tooltips. The model exposes a
  `fileName` role specifically for QML so filenames do not collide with QML
  object naming and stay visible in the tree.
* Right-click opens a `Menu`: 打开到编辑器 / 复制 / 粘贴 / 重命名 / 删除 / 新建文件夹 /
  上传到此处.
* The center workspace has tabs for **终端** and **编辑器**. Double-clicking a row
  where `editable` is true switches the center workspace to the editor tab and
  calls `EditorViewModel.open(path)`.
* The **编辑器** tab is a VS Code-style text editor surface: file tab title,
  path header, monospaced editing area, gutter, modified/saving state, and bottom
  status bar. The editable text still lives in QML and saves via `fs_write`.
* Ctrl+S saves the current editor buffer. The modified marker clears only after a
  successful save callback; save failure leaves the buffer marked modified.
* Opening another file with unsaved changes prompts before discarding the buffer.
* Delete actions require confirmation; directory deletion is recursive.
* Upload, paste, and rename show an overwrite confirmation when the destination
  name is already visible in the loaded file tree. New-folder creation refuses
  visible duplicate names.
* While file requests are pending, the file tab shows a compact busy indicator
  with the number of outstanding operations.
* If SFTP is unavailable or root listing fails, the file tab shows the current
  file-capability status and disables mutating file actions.
* The editor rejects obvious binary/non-UTF-8 content and refuses files at the
  current 2 MiB lightweight-editor read cap.
* The tree/editor never see handles or secrets — only `app`-exposed models/slots.

## Data flows

* **Browse / expand**: expand dir → `fs_list(path)` → `Listing` result → populate
  children. Root is listed on connect with an empty path, which providers resolve
  to the session's home directory.
* **Open & edit**: double-click text file → `fs_read` → `Content` → editor shows
  text → user edits (`modified=true`) → 保存 → `fs_write` → `Ok` → `modified=false`.
* **Upload**: pick local file → read locally → `fs_write(remoteDir/name, bytes)` →
  `Ok` → re-list the directory.
* **Rename / copy(paste) / delete / mkdir**: call the op → `Ok`/`Error` → re-list
  the affected parent; surface errors as a terminal notice.

## Real backend: `russh-sftp`

With `--features russh`, file management is implemented by opening an **SFTP
subsystem** on a second channel and mapping each `FileProvider` method to a
`russh-sftp` client request:

1. `russh-sftp` is an optional dependency under the `russh` feature.
2. `RusshProvider::connect` tries to open the SFTP subsystem after the shell is
   established; failure is non-fatal.
3. Map: `list_dir`→read_dir, `stat`→metadata, `read_file`→read,
   `write_file`→open(create/trunc)+write, `rename`→rename, `remove`→remove/rmdir
   (recursive walk for directories), `mkdir`→mkdir, `copy`→read+write.
4. Relative/empty remote paths resolve to the canonical home directory in the
   SFTP provider.
5. Missing SFTP capability maps to `RsErrorCode::ProviderUnavailable`; concrete
   SFTP failures map to `RsErrorCode::RuntimeError` with stage-specific messages
   and common hints such as permission denied, path not found, target exists, or
   directory not empty.

No layer above the provider changes — the FFI, C++ models and QML are all written
against the abstraction defined here.

## Implementation checklist (for the build-it pass)

- [x] `session.rs`: hold an optional `Box<dyn FileProvider>`; add file commands +
      a file-result channel; assign request ids.
- [x] `ffi.rs`: add the types/functions above; `catch_unwind`; batched file callback;
      copy-out ownership; regenerate the header.
- [x] `RustCoreBridge`: file callback trampoline → `Qt::QueuedConnection`.
- [x] `RemoteFileTreeModel`, `EditorViewModel`, file actions in `AppController`.
- [x] QML: `TreeView` pane, context `Menu`, editor pane, upload dialog.
- [x] Mock provider: in-memory file provider.
- [x] (with `russh`) `providers`: `SftpFileProvider` via `russh-sftp`.
- [x] Tests: FFI smoke test for listing + read/write round-trip against Mock.
- [x] Harden editor UX pass 1: binary/UTF-8 rejection, large-file guard,
      unsaved-change prompt, delete confirmation, Ctrl+S shortcut, and save-failure
      modified-state preservation.
- [x] Harden editor UX pass 2: visible duplicate-name overwrite prompts and
      pending-operation status.
- [x] Harden file capability pass: unavailable-SFTP status, root/home write-path
      normalization, and clearer SFTP error hints.
- [x] UI pass: center workspace terminal/editor tabs, larger VS Code-style editor,
      and clearer file explorer rows.
- [x] Theme pass: global QML theme singleton with header toggle for dark/light
      mode, applied across the main panes, dialogs, file explorer, terminal, and
      editor.
- [ ] Harden editor UX pass 3: byte-level progress UI and operation cancel for
      large reads/writes. This requires extending the file-operation API beyond
      one-shot read/write buffers.
