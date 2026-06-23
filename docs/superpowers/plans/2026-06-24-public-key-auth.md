# Public-Key Authentication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire public-key authentication (auto-discovered or user-specified key, optional passphrase, password fallback) end-to-end through the russh backend, C ABI, and Qt UI.

**Architecture:** Thread one new credential — a private-key candidate (`Option<path>` + `Option<passphrase Secret>`) — from the UI to the provider in parallel to the existing password `Secret`. The russh provider gains "public-key first, fall back to password" ordering; everything above it stays provider-agnostic. The C ABI gains exactly one additive function.

**Tech Stack:** Rust (tokio, russh 0.61, russh-sftp, async-trait, zeroize), C ABI via cbindgen, C++/Qt 6.8 (Quick/QML).

## Global Constraints

- Rust edition 2021, `rust-version = 1.75`. Crate-root `#![deny(unsafe_code)]`; only the `ffi` module is `#[allow(unsafe_code)]`.
- C ABI changes are **additive only** — do not modify any existing `#[repr(C)]` struct or existing function signature in `ffi.rs` / `research_ssh_core.h`.
- Secrets (password, passphrase) travel only through the zeroizing `Secret` channel; they never appear in a `#[repr(C)]` struct or in QML state beyond the input field.
- The real SSH backend is feature-gated: `russh.rs` compiles only under `--features russh`; the default build wires the mock only.
- russh API facts (use verbatim):
  - `russh::keys::load_secret_key(path, passphrase: Option<&str>) -> Result<PrivateKey, _>`
  - `russh::keys::{HashAlg, PrivateKey}` (re-exports of `ssh_key`).
  - `PrivateKeyWithHashAlg::new(Arc<PrivateKey>, Some(HashAlg::Sha256))` — for non-RSA keys the hash is auto-set to `None`; passing `Some(Sha256)` is correct for all keys.
  - `session.authenticate_publickey(user, kwh).await? .success()` returns `bool`.
  - Server side (tests): `Handler::auth_publickey(&mut self, user: &str, key: &ssh_key::PublicKey) -> Result<Auth>`; `PrivateKey::write_openssh_file(path, LineEnding::LF)`, `PrivateKey::public_key() -> &PublicKey`.
- Commits must NOT include any `Co-Authored-By` trailer (user global rule).
- Verify Rust with `cargo test` (default) and `cargo test --features russh`; verify C++ with the full Qt build target `researchssh_next`.

---

## File Structure

- `rust-core/src/provider.rs` — add `set_private_key` to the `SshProvider` trait (default no-op).
- `rust-core/src/session.rs` — add `Command::SetPrivateKey`, `Session::set_private_key`, driver arm.
- `rust-core/src/ffi.rs` — add `rscore_session_set_private_key` (+ tests).
- `rust-core/src/providers/russh.rs` — key discovery helpers, provider fields, `set_private_key` impl, public-key-first `connect()`, e2e tests.
- `app/cpp/RustCoreBridge.{h,cpp}` — add `setSessionPrivateKey`.
- `app/cpp/ServerListModel.{h,cpp}` — `ServerItem.keyPath`, `addServer` param.
- `app/cpp/AppController.{h,cpp}` — extend `connectToHost`, dispatch key in `connectToServer`.
- `app/qml/components/ConnectDialog.qml` — key-file + passphrase fields.

---

## Task 1: Rust credential pipeline (trait + command + FFI entry; mock ignores it)

**Files:**
- Modify: `rust-core/src/provider.rs` (SshProvider trait)
- Modify: `rust-core/src/session.rs` (Command, Session method, driver arm)
- Modify: `rust-core/src/ffi.rs` (new entry + tests)

**Interfaces:**
- Produces: `SshProvider::set_private_key(&mut self, key_path: Option<String>, passphrase: Option<Secret>)` (default no-op); `Session::set_private_key(&self, Option<String>, Option<Secret>) -> CoreResult<()>`; `extern "C" fn rscore_session_set_private_key(session: *mut RsSession, key_path: *const c_char, passphrase: *const u8, passphrase_len: usize) -> RsErrorCode`.

- [ ] **Step 1: Add the trait method** in `provider.rs`, right after `set_secret`:

```rust
    /// Supply a private-key candidate for public-key auth. `key_path = None`
    /// means "auto-discover ~/.ssh defaults". Default impl ignores it (the mock
    /// does not authenticate).
    fn set_private_key(&mut self, _key_path: Option<String>, _passphrase: Option<Secret>) {}
```

- [ ] **Step 2: Add the command + driver handling** in `session.rs`.

In the `Command` enum, after `SetSecret(Secret)`:
```rust
    SetPrivateKey {
        key_path: Option<String>,
        passphrase: Option<Secret>,
    },
```
Add a `Session` method next to `set_secret`:
```rust
    /// Hand a private-key candidate (path + optional passphrase) to the provider.
    pub fn set_private_key(
        &self,
        key_path: Option<String>,
        passphrase: Option<Secret>,
    ) -> CoreResult<()> {
        self.send_cmd(Command::SetPrivateKey { key_path, passphrase })
    }
```
In `drive`'s command `match`, next to the `SetSecret` arm:
```rust
                    Some(Command::SetPrivateKey { key_path, passphrase }) => {
                        provider.set_private_key(key_path, passphrase);
                    }
```

- [ ] **Step 3: Add the FFI entry** in `ffi.rs`, after `rscore_session_set_password`:

```rust
/// Supplies a private key for public-key authentication. `key_path` null or
/// empty means "auto-discover the user's ~/.ssh default keys". `passphrase`
/// null / `passphrase_len == 0` means the key has no passphrase; otherwise the
/// bytes are copied into a zeroizing buffer immediately. Symmetric with
/// [`rscore_session_set_password`].
#[no_mangle]
pub extern "C" fn rscore_session_set_private_key(
    session: *mut RsSession,
    key_path: *const c_char,
    passphrase: *const u8,
    passphrase_len: usize,
) -> RsErrorCode {
    guard_code(|| {
        let key_path = if key_path.is_null() {
            None
        } else {
            match cstr_to_string(key_path) {
                Ok(s) if s.is_empty() => None,
                Ok(s) => Some(s),
                Err(code) => return code,
            }
        };
        let passphrase = if passphrase_len == 0 {
            None
        } else {
            match bytes_to_vec(passphrase, passphrase_len) {
                Ok(b) => Some(Secret::from_bytes(&b)),
                Err(code) => return code,
            }
        };
        with_session(session, |s| {
            s.set_private_key(key_path, passphrase);
            Ok(())
        })
    })
}
```

- [ ] **Step 4: Write the test** in `ffi.rs`'s `mod tests`, after `null_handles_are_rejected_not_crashed`:

```rust
    #[test]
    fn set_private_key_accepts_path_passphrase_and_null() {
        let mut err = RsErrorCode::Internal;
        let core = rscore_create(&mut err);
        let host = CString::new("hpc.example.edu").unwrap();
        let user = CString::new("researcher").unwrap();
        let config = RsSessionConfig {
            host: host.as_ptr(),
            port: 22,
            username: user.as_ptr(),
            provider: RsProviderKind::Mock,
        };
        let session =
            rscore_session_create(core, &config, Some(noop_events), ptr::null_mut(), &mut err);
        assert!(!session.is_null());

        let path = CString::new("/home/researcher/.ssh/id_ed25519").unwrap();
        let pass = b"s3cret";
        // explicit path + passphrase
        assert_eq!(
            rscore_session_set_private_key(session, path.as_ptr(), pass.as_ptr(), pass.len()),
            RsErrorCode::Ok
        );
        // null path (auto-discover) + no passphrase
        assert_eq!(
            rscore_session_set_private_key(session, ptr::null(), ptr::null(), 0),
            RsErrorCode::Ok
        );
        // null handle is rejected, not crashed
        assert_eq!(
            rscore_session_set_private_key(ptr::null_mut(), path.as_ptr(), ptr::null(), 0),
            RsErrorCode::NullArgument
        );

        rscore_session_destroy(session);
        rscore_destroy(core);
    }
```

- [ ] **Step 5: Run tests, expect FAIL then PASS**

Run: `cd rust-core && cargo test set_private_key_accepts -- --nocapture`
First expectation if you wrote the test before the impl: compile error / fail. After Steps 1-3 it PASSES. Also run the full default suite: `cargo test` (expect 19 passed).

- [ ] **Step 6: Commit**

```bash
git add rust-core/src/provider.rs rust-core/src/session.rs rust-core/src/ffi.rs
git commit -m "feat(core): add private-key credential pipeline (trait + command + FFI)"
```

---

## Task 2: Key discovery helpers (russh.rs, pure + env)

**Files:**
- Modify: `rust-core/src/providers/russh.rs` (add helpers + unit test)

**Interfaces:**
- Produces: `fn default_ssh_dir() -> Option<PathBuf>`; `fn keys_in_dir(ssh_dir: &Path) -> Vec<PathBuf>`.

- [ ] **Step 1: Add imports** at the top of `russh.rs` if not present: `use std::path::{Path, PathBuf};`

- [ ] **Step 2: Add the helpers** near the other free functions (e.g. after `join_path`):

```rust
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
```

- [ ] **Step 3: Write the unit test** in a new `#[cfg(test)] mod` in `russh.rs` (next to `kind_tests`):

```rust
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
```

- [ ] **Step 4: Run the test**

Run: `cd rust-core && cargo test --features russh keys_in_dir_returns -- --nocapture`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add rust-core/src/providers/russh.rs
git commit -m "feat(russh): add ~/.ssh default-key discovery helpers"
```

---

## Task 3: russh public-key auth (provider fields + set_private_key + connect ordering + e2e)

**Files:**
- Modify: `rust-core/src/providers/russh.rs`

**Interfaces:**
- Consumes: `default_ssh_dir`, `keys_in_dir` (Task 2); `SshProvider::set_private_key` (Task 1).
- Produces: working public-key-first authentication in `RusshProvider::connect`.

- [ ] **Step 1: Add imports** to `russh.rs`: `use std::sync::Arc;` (already present), and `use russh::keys::{HashAlg, PrivateKey, PrivateKeyWithHashAlg, PublicKey};` (extend the existing `use russh::keys::{HashAlg, PublicKey};` line to include `PrivateKey, PrivateKeyWithHashAlg`).

- [ ] **Step 2: Add provider fields.** In `struct RusshProvider`, after `secret: Option<Secret>,`:

```rust
    key_path: Option<String>,
    key_passphrase: Option<Secret>,
```
In `RusshProvider::new`, initialize both to `None` (add `key_path: None, key_passphrase: None,` to the struct literal).

- [ ] **Step 3: Implement `set_private_key`** in `impl SshProvider for RusshProvider`, next to `set_secret`:

```rust
    fn set_private_key(&mut self, key_path: Option<String>, passphrase: Option<Secret>) {
        self.key_path = key_path;
        self.key_passphrase = passphrase;
    }
```

- [ ] **Step 4: Add an auth helper** (free function in `russh.rs`):

```rust
/// Try public-key auth against `session` for every candidate key, in order.
/// Returns `Ok(true)` on the first key the server accepts, `Ok(false)` if none
/// were accepted (caller may then fall back to password). Per-key load/auth
/// failures are swallowed so the next candidate is tried.
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
```

- [ ] **Step 5: Rewrite the auth section of `connect`.** Replace the current password block (the `let password = match &self.secret { ... }` and the `authenticate_password` + `if !auth.success()` block) with:

```rust
        // Public-key first: explicit key path overrides auto-discovery.
        let candidates: Vec<PathBuf> = match &self.key_path {
            Some(p) => vec![PathBuf::from(p)],
            None => default_ssh_dir()
                .map(|d| keys_in_dir(&d))
                .unwrap_or_default(),
        };
        let passphrase: Option<String> = match &self.key_passphrase {
            Some(s) => Some(String::from_utf8(s.expose().to_vec()).map_err(|_| {
                CoreError::with_detail(RsErrorCode::ConnectFailed, "key passphrase is not valid UTF-8")
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
                    CoreError::with_detail(RsErrorCode::ConnectFailed, "password is not valid UTF-8")
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
```

- [ ] **Step 6: Extend the e2e test server** in `mod e2e` to trust a key and accept it. Change `struct ShellHandler;` to carry a trusted key, and give the `Server` impl a key to clone:

`trusted` is `Option<PublicKey>` so the existing password test keeps a unit-like
construction (`None`):
```rust
    struct Srv {
        trusted: Option<PublicKey>,
    }
    impl server::Server for Srv {
        type Handler = ShellHandler;
        fn new_client(&mut self, _peer: Option<std::net::SocketAddr>) -> ShellHandler {
            ShellHandler { trusted: self.trusted.clone() }
        }
    }

    struct ShellHandler {
        trusted: Option<PublicKey>,
    }
```
Add the imports the test module needs (inside `mod e2e`): `use russh::keys::{PrivateKey, PublicKey}; use ssh_key::LineEnding;` and in the `impl server::Handler for ShellHandler` add:
```rust
        async fn auth_publickey(
            &mut self,
            _user: &str,
            key: &ssh_key::PublicKey,
        ) -> Result<Auth, Self::Error> {
            match &self.trusted {
                Some(t) if key == t => Ok(Auth::Accept),
                _ => Ok(Auth::reject()),
            }
        }
```
(Keep `auth_password` as-is for the fallback test.)

**Also update the existing password test** `russh_provider_real_handshake_over_loopback`: change its `let mut srv = Srv;` to `let mut srv = Srv { trusted: None };` so it still compiles.

- [ ] **Step 7: Add a public-key e2e test** in `mod e2e` (mirror the existing handshake test but authenticate with a key). Generate a client key, trust its public half on the server, write the private key to a temp file, and drive it through `rscore_session_set_private_key`:

```rust
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
            .write_openssh_file(&key_path, ssh_key::LineEnding::LF)
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
                let mut srv = Srv { trusted: Some(trusted) };
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

        // Explicit key path; no password set.
        let key_path_c = CString::new(key_path.to_string_lossy().as_ref()).unwrap();
        assert_eq!(
            crate::ffi::rscore_session_set_private_key(
                session,
                key_path_c.as_ptr(),
                std::ptr::null(),
                0
            ),
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
```
Add `rscore_session_set_private_key` to the `use crate::ffi::{...}` import list at the top of `mod e2e` (or call it fully-qualified as written above).

- [ ] **Step 8: Run the russh suite**

Run: `cd rust-core && cargo test --features russh -- --nocapture`
Expected: all pass, including `russh_provider_publickey_auth_over_loopback` and the existing `russh_provider_real_handshake_over_loopback` (password) — confirming both paths work.

- [ ] **Step 9: Commit**

```bash
git add rust-core/src/providers/russh.rs
git commit -m "feat(russh): public-key-first authentication with password fallback"
```

---

## Task 4: C++ bridge + models + controller

**Files:**
- Modify: `app/cpp/RustCoreBridge.h`, `app/cpp/RustCoreBridge.cpp`
- Modify: `app/cpp/ServerListModel.h`, `app/cpp/ServerListModel.cpp`
- Modify: `app/cpp/AppController.h`, `app/cpp/AppController.cpp`

**Interfaces:**
- Consumes: `rscore_session_set_private_key` (Task 1).
- Produces: `RustCoreBridge::setSessionPrivateKey(RsSession*, const QString&, const QByteArray&)`; `ServerItem.keyPath`; extended `AppController::connectToHost(..., const QString& keyPath, const QString& keyPassphrase)`.

- [ ] **Step 1: Bridge declaration** — in `RustCoreBridge.h`, after `setSessionPassword`:
```cpp
    RsErrorCode setSessionPrivateKey(RsSession *session, const QString &keyPath,
                                     const QByteArray &passphrase);
```

- [ ] **Step 2: Bridge definition** — in `RustCoreBridge.cpp`, after `setSessionPassword`:
```cpp
RsErrorCode RustCoreBridge::setSessionPrivateKey(RsSession *session, const QString &keyPath,
                                                 const QByteArray &passphrase) {
    const QByteArray pathUtf8 = keyPath.toUtf8();
    return rscore_session_set_private_key(
        session, pathUtf8.constData(),
        reinterpret_cast<const uint8_t *>(passphrase.constData()),
        static_cast<uintptr_t>(passphrase.size()));
}
```
(Empty `keyPath` yields an empty C string → the core treats it as auto-discover.)

- [ ] **Step 3: ServerItem field** — in `ServerListModel.h`, add to `struct ServerItem` after `username`:
```cpp
    QString keyPath; // optional private-key path for public-key auth ("" = auto-discover)
```
Change `addServer` signature to:
```cpp
    Q_INVOKABLE int addServer(const QString &name, const QString &host, int port,
                              const QString &username, int provider,
                              const QString &keyPath = QString());
```

- [ ] **Step 4: addServer definition** — in `ServerListModel.cpp`, set `item.keyPath = keyPath;` where the new `ServerItem` is populated (add the parameter `const QString &keyPath` to the definition signature to match the header).

- [ ] **Step 5: Controller signature** — in `AppController.h`, update the slot:
```cpp
    void connectToHost(const QString &host, int port, const QString &username,
                       const QString &password, const QString &name,
                       const QString &keyPath, const QString &keyPassphrase);
```

- [ ] **Step 6: Controller body** — in `AppController.cpp`, replace `connectToHost` with:
```cpp
void AppController::connectToHost(const QString &host, int port, const QString &username,
                                  const QString &password, const QString &name,
                                  const QString &keyPath, const QString &keyPassphrase) {
    if (host.isEmpty() || username.isEmpty()) {
        m_terminal->appendNotice(QStringLiteral("主机和用户名不能为空。"));
        return;
    }
    const quint16 p = port > 0 ? static_cast<quint16>(port) : 22;
    const int index = m_servers->addServer(name, host, p, username,
                                            static_cast<int>(RsProviderKind_Russh), keyPath);

    const QString endpoint = QStringLiteral("%1@%2:%3").arg(username, host).arg(p);
    if (!password.isEmpty())
        m_credentials->store(endpoint, password.toUtf8());
    if (!keyPassphrase.isEmpty())
        m_credentials->store(endpoint + QStringLiteral("#keypass"), keyPassphrase.toUtf8());

    connectToServer(index);
}
```

- [ ] **Step 7: Dispatch the key in connectToServer** — in `AppController.cpp`, inside `connectToServer`, in the `if (!m_session)` block right after the existing password block (`const QByteArray secret = m_credentials->load(...)` ... `setSessionPassword`), add:
```cpp
        // Public-key auth: hand the (optional) key path + passphrase to the core.
        // An empty path means auto-discover ~/.ssh defaults; the mock ignores this.
        const ServerItem &serverItem = m_servers->itemAt(index);
        const QByteArray keyPass = m_credentials->load(m_currentEndpoint + QStringLiteral("#keypass"));
        m_bridge.setSessionPrivateKey(m_session, serverItem.keyPath, keyPass);
```

- [ ] **Step 8: Build the app**

Run: `cmake --build build/windows-msvc-russh --config Release --target researchssh_next`
Expected: links to `researchssh_next.exe` with no errors.

- [ ] **Step 9: Commit**

```bash
git add app/cpp/RustCoreBridge.h app/cpp/RustCoreBridge.cpp app/cpp/ServerListModel.h app/cpp/ServerListModel.cpp app/cpp/AppController.h app/cpp/AppController.cpp
git commit -m "feat(app): plumb private-key path + passphrase to the core"
```

---

## Task 5: ConnectDialog UI (key file + passphrase fields)

**Files:**
- Modify: `app/qml/components/ConnectDialog.qml`

**Interfaces:**
- Consumes: `AppController::connectToHost(host, port, user, password, name, keyPath, keyPassphrase)` (Task 4).

- [ ] **Step 1: Import FileDialog** — add to the imports at the top of `ConnectDialog.qml`:
```qml
import QtQuick.Dialogs
```

- [ ] **Step 2: Reset new fields** — in `resetFields()`, after `passField.text = ""`:
```qml
        keyPathField.text = ""
        keyPassField.text = ""
```

- [ ] **Step 3: Add the two rows** in the `GridLayout`, after the password `TextField` (id `passField`) row:
```qml
            Text { text: "私钥文件"; color: Theme.muted; font.pixelSize: 13 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                TextField {
                    id: keyPathField
                    Layout.fillWidth: true
                    placeholderText: "留空则自动发现 ~/.ssh"
                    color: Theme.text
                    placeholderTextColor: Theme.faint
                    background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: keyPathField.activeFocus ? Theme.accent : Theme.border }
                }
                StyledButton {
                    text: "浏览…"
                    onClicked: keyFileDialog.open()
                }
            }

            Text { text: "密钥口令"; color: Theme.muted; font.pixelSize: 13 }
            TextField {
                id: keyPassField
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: "加密私钥才需要"
                color: Theme.text
                placeholderTextColor: Theme.faint
                background: Rectangle { radius: 6; color: Theme.panelAlt; border.color: keyPassField.activeFocus ? Theme.accent : Theme.border }
            }
```

- [ ] **Step 4: Add the FileDialog** as a child of the root `Dialog` (e.g. right before the closing brace of `contentItem`'s siblings — place it after `contentItem: ColumnLayout { ... }`):
```qml
    FileDialog {
        id: keyFileDialog
        title: "选择私钥文件"
        onAccepted: keyPathField.text = selectedFile.toString().replace("file:///", "")
    }
```

- [ ] **Step 5: Pass the new args** in `connectButton.onClicked` — update the call:
```qml
                onClicked: {
                    dialog.controller.connectToHost(hostField.text,
                                                    parseInt(portField.text) || 22,
                                                    userField.text,
                                                    passField.text, "",
                                                    keyPathField.text,
                                                    keyPassField.text)
                    passField.text = ""
                    keyPassField.text = ""
                    dialog.close()
                }
```

- [ ] **Step 6: Build the app**

Run: `cmake --build build/windows-msvc-russh --config Release --target researchssh_next`
Expected: links cleanly (QML is compiled into the binary via qmlcachegen).

- [ ] **Step 7: Commit**

```bash
git add app/qml/components/ConnectDialog.qml
git commit -m "feat(ui): add private-key file + passphrase fields to ConnectDialog"
```

---

## Final verification

- [ ] `cd rust-core && cargo test` (default, expect all pass)
- [ ] `cd rust-core && cargo test --features russh` (expect all pass incl. both e2e tests)
- [ ] `cd rust-core && cargo clippy --features russh --all-targets` (no new warnings)
- [ ] `cmake --build build/windows-msvc-russh --config Release --target researchssh_next` (links)
- [ ] Confirm `rust-core/include/research_ssh_core.h` gained `rscore_session_set_private_key` and nothing else changed.
