# Public-Key Authentication — Design Spec

Date: 2026-06-24
Status: Approved (brainstorming) — ready for implementation plan
Scope: One sub-project from the roadmap (`docs/architecture.md` → "Future work").

## 1. Context & goals

The `russh` backend currently authenticates with password only
(`rust-core/src/providers/russh.rs`); the connect path bails out with
"no password set (public-key auth not wired yet)". Researchers / HPC users
predominantly log in with SSH keys, so this sub-project wires public-key
authentication end-to-end (Rust core → C ABI → C++/QML), while keeping the
existing password flow working as a fallback.

Success criteria:
- A russh session can authenticate with a private key discovered in `~/.ssh`
  or with a user-specified key file, optionally protected by a passphrase.
- If public-key auth does not succeed, the session falls back to password auth
  when a password was supplied.
- Secrets (passphrase, password) never pass through QML state and travel only
  through the zeroizing `Secret` channel.
- The C ABI change is purely additive (existing `#[repr(C)]` structs and
  functions are untouched).

## 2. Decisions (from brainstorming)

1. **Key source**: auto-discover `~/.ssh` defaults (`id_ed25519`, `id_ecdsa`,
   `id_rsa`, in that preference order) **plus** an optional user-specified key
   path that overrides discovery.
2. **Auth strategy**: public-key first, fall back to password (OpenSSH-like).
3. **Passphrase input**: a dedicated "key passphrase" field, separate from the
   login password, carried over the zeroizing `Secret` channel.

## 3. Architecture overview

The change threads one new piece of state — a private-key candidate
(`Option<path>` + `Option<passphrase Secret>`) — from the UI to the provider,
parallel to the existing password `Secret`. No new threading model, no new
event kinds. The provider gains the auth-ordering logic; everything above it is
provider-agnostic.

Affected files:
- Rust: `provider.rs`, `providers/russh.rs`, `session.rs`, `ffi.rs`
  (+ generated `include/research_ssh_core.h` via cbindgen), `providers/mock.rs`
  (only if the trait default needs acknowledging — default impl means no change).
- C++: `RustCoreBridge.{h,cpp}`, `AppController.{h,cpp}`,
  `ServerListModel.{h,cpp}`, `CredentialStore` usage (no interface change).
- QML: `ConnectDialog.qml`.
- Tests: `providers/russh.rs` (e2e + unit), full Qt build for the C++ side.

## 4. Rust core changes

### 4.1 Provider trait (`provider.rs`)

Add one default-no-op method; the existing `set_secret` (password) is unchanged:

```rust
/// Supply a private-key candidate for public-key auth. `key_path = None` means
/// "auto-discover ~/.ssh defaults". Default impl ignores it (e.g. the mock).
fn set_private_key(&mut self, key_path: Option<String>, passphrase: Option<Secret>) {}
```

### 4.2 russh backend (`providers/russh.rs`)

New fields on `RusshProvider`: `key_path: Option<String>`,
`key_passphrase: Option<Secret>`. Implement `set_private_key` to store them.

New module-private helpers, split so discovery is a pure, env-free function
(testable without touching process-global `HOME`, which the loopback e2e also
mutates — see §8):

```rust
/// The ~/.ssh directory from USERPROFILE (Windows) else HOME, if any.
fn default_ssh_dir() -> Option<PathBuf>;

/// Existing default identity files in `ssh_dir`, in OpenSSH preference order:
/// id_ed25519, id_ecdsa, id_rsa — only those that exist, in that order.
fn keys_in_dir(ssh_dir: &Path) -> Vec<PathBuf>;
```

`connect()` authentication section becomes "public-key first, then password":

1. `candidates = match &self.key_path { Some(p) => vec![PathBuf::from(p)],
   None => default_ssh_dir().map(|d| keys_in_dir(&d)).unwrap_or_default() }`.
2. For each candidate:
   - `passphrase: Option<String>` from `self.key_passphrase` (UTF-8 via
     `String::from_utf8`; non-UTF-8 → treat as an invalid passphrase for this
     key and move on, same rule as the password path).
   - `russh::keys::load_secret_key(path, passphrase.as_deref())`. On error
     (bad passphrase / parse error) record it and try the next candidate.
   - `let kwh = PrivateKeyWithHashAlg::new(Arc::new(key), hash_alg)` where
     `hash_alg = Some(HashAlg::Sha256)` for RSA keys, else `None`
     (rsa-sha2-256; plain ssh-rsa/SHA-1 is deprecated).
   - `session.authenticate_publickey(&self.config.username, kwh).await`; on
     `Ok(r)` with `r.success()` → authenticated, stop. Otherwise try next.
3. If still not authenticated and `self.secret` (password) is set, run the
   existing `authenticate_password` path.
4. If nothing succeeded → `CoreError(ConnectFailed, ...)` with a message that
   names which methods were attempted, e.g.
   "authentication failed (public-key and password)". If there were no
   candidates and no password → "no usable key or password".

Notes / honest limitations:
- `load_secret_key` takes `&str`, so russh holds a plaintext `String`
  passphrase copy that cannot be zeroized — a library API limitation, identical
  to the existing password path. A comment documents this.
- Exact RSA-detection API (`key.algorithm()` vs a helper) is confirmed during
  implementation; the rule "RSA → SHA-256, otherwise None" is fixed.

### 4.3 Session & commands (`session.rs`)

- `Command` gains `SetPrivateKey { key_path: Option<String>, passphrase: Option<Secret> }`.
- `Session::set_private_key(&self, key_path, passphrase) -> CoreResult<()>`
  sends that command (mirrors `set_secret`).
- The driver handles `SetPrivateKey` by calling
  `provider.set_private_key(key_path, passphrase)` (same shape as the existing
  `SetSecret` arm).

### 4.4 FFI (`ffi.rs`) — additive only

```rust
/// Supply a private key for public-key auth. `key_path` null/empty = auto-
/// discover ~/.ssh defaults. `passphrase` null/len 0 = no passphrase. The
/// passphrase bytes are copied into a zeroizing buffer immediately.
#[no_mangle]
pub extern "C" fn rscore_session_set_private_key(
    session: *mut RsSession,
    key_path: *const c_char,
    passphrase: *const u8,
    passphrase_len: usize,
) -> RsErrorCode;
```

Implementation: parse `key_path` (null/empty → `None`), build
`Option<Secret>` from the passphrase bytes via `bytes_to_vec` +
`Secret::from_bytes` (symmetric with `rscore_session_set_password`), then
`with_session(session, |s| s.set_private_key(path, passphrase))`. Wrapped in
`guard_code`. cbindgen emits the prototype into
`include/research_ssh_core.h`; no existing struct/function changes.

## 5. C++ / QML changes

### 5.1 ConnectDialog.qml

Below the password field, add two optional fields:
- **私钥文件** (key file): a path `TextField` plus a "浏览…" button opening a
  `QtQuick.Dialogs` `FileDialog`; the chosen path is written back into the
  field. Empty = auto-discover.
- **密钥口令** (key passphrase): `echoMode: TextInput.Password`. Empty = none.

The connect button calls the extended
`controller.connectToHost(host, port, user, password, name, keyPath, keyPassphrase)`
and clears the passphrase field afterwards (as it already does for password).

### 5.2 ServerListModel

`ServerItem` gains `QString keyPath;` (non-secret, retained for reconnect).
`addServer(...)` gains a `keyPath` parameter. Demo servers leave it empty (no
effect — they use the mock provider). No new role is required.

### 5.3 RustCoreBridge

```cpp
RsErrorCode setSessionPrivateKey(RsSession *session, const QString &keyPath,
                                 const QByteArray &passphrase);
```
Calls `rscore_session_set_private_key` (`keyPath.toUtf8()`, empty string is
fine = auto-discover; passphrase bytes + size).

### 5.4 AppController

- `connectToHost` gains `const QString &keyPath, const QString &keyPassphrase`.
- Storage split: `keyPath` → `ServerItem` (via `addServer`); `password` →
  CredentialStore (existing); `passphrase` → CredentialStore under
  `endpoint + "#keypass"` so it is distinct from the password entry. The
  passphrase never lingers in QML state beyond the input field.
- `connectToServer`: after `createSession`, if a password exists call
  `setSessionPassword`; for russh sessions always call
  `setSessionPrivateKey(item.keyPath, passphrase)` (empty `keyPath` triggers
  auto-discovery). The mock provider ignores it via the trait default.

### 5.5 CredentialStore

No interface change. Two keys per endpoint: `endpoint` (password) and
`endpoint + "#keypass"` (passphrase). Both are wiped by the existing
deep-copy + fill logic.

## 6. Data flow

```
ConnectDialog
  → connectToHost(host, port, user, password, name, keyPath, keyPassphrase)
    → addServer(..., keyPath)
    → store(endpoint, password); store(endpoint#keypass, passphrase)
    → connectToServer(index)
      → createSession + setFileCallback
      → if password: setSessionPassword(password)
      → setSessionPrivateKey(item.keyPath, passphrase)   // empty path = discover
      → connectSession
        → [core] driver: SetSecret / SetPrivateKey → provider stores them
        → provider.connect(): try each key candidate → fall back to password
        → Connected  OR  Error(ConnectFailed)
```

## 7. Error handling

Surfaced through the existing error event stream (terminal notice + server row
status → Failed); no new event kind:
- Public-key and password both failed → "认证失败（公钥与密码均失败）".
- Specified key file missing and no password → `ConnectFailed`
  "无可用密钥或密码".
- Wrong passphrase → that key fails to load; fall back or final failure with a
  hint that the passphrase may be wrong.
- Never echo key contents or the passphrase in any message/log.

## 8. Testing

1. **Unit — `keys_in_dir`**: create a temp dir with `id_ed25519` and `id_rsa`
   (no `id_ecdsa`), assert the returned order (`id_ed25519` before `id_rsa`) and
   that only existing files appear. Pure function, no env mutation → safe to run
   in parallel with the env-mutating e2e test.
2. **e2e — public-key handshake** (extends the loopback test in
   `providers/russh.rs`): generate an ed25519 test pair; the in-process server's
   `auth_publickey` trusts that public key; write the private key to a temp
   file; drive the client over the C ABI with `rscore_session_set_private_key`
   (explicit path) and assert `Connected` + banner.
3. **e2e — password fallback** (optional but recommended): server rejects the
   key but accepts the password; assert the session still reaches `Connected`.
4. **C++ side**: verified by a full Qt build (`researchssh_next`).

## 9. Out of scope (future sub-projects)

- ssh-agent integration.
- Certificate-based auth and richer `ssh_config` parsing.
- Keyboard-interactive auth.
- Persisting key choices beyond the in-memory mock credential store (real
  platform keystores are a separate roadmap item).

## 10. Compatibility

- C ABI: additive only. Existing callers keep working; the generated header
  gains one prototype.
- Default-feature build (mock only) is unaffected: the trait default makes
  `set_private_key` a no-op and the new FFI entry simply stores state the mock
  ignores.
