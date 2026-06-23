# ResearchSSH-Next

A **Windows-first, Android-ready** professional SSH client for research workflows.

**Architecture:** a **Rust security core** (SSH/SFTP, sessions, scheduling, async)
behind a stable **C ABI**, driven by a **C++20 / Qt 6 Quick (QML)** UI. No Qt
Widgets, no WebView, no Electron/Tauri.

> This is still an early product stage, but the core path is now usable end to
> end. A built-in **Mock provider** drives the demo UI without network access, and
> the optional `russh` provider enables real SSH/password login, host-key
> confirmation, PTY shell and best-effort SFTP.

---

## Current status

| Component | Status |
|-----------|--------|
| Rust core (runtime, session FSM, provider abstraction, events) | ✅ implemented |
| C ABI (cbindgen header, opaque handles, `catch_unwind`, batched callbacks) | ✅ implemented |
| Mock SSH provider | ✅ implemented |
| `russh` provider (real SSH: handshake, password auth, host-key confirm, PTY+shell) | ✅ implemented behind `--features russh`; verified by an in-process server e2e test |
| Connect-to-real-server UI (connection dialog + host-key confirm dialog) | ✅ implemented |
| Remote file mgmt + in-app editor (file tree / copy-paste-rename / upload / edit-save) | ✅ implemented for Mock; wired through FFI/C++/QML; `russh` attempts SFTP when the server supports it |
| Rust unit tests + FFI smoke test + Mock session test | ✅ **build & pass** |
| C++/Qt adapter (AppController, models, RustCoreBridge, CredentialStore) | ✅ implemented |
| QML dark three-pane UI (servers · terminal · workspace tabs: connection/files) | ✅ implemented |
| CMake + Cargo build, CMakePresets | ✅ implemented |
| Full terminal emulator / public-key auth / port forwarding | ⛔ follow-up work |

### What has been verified on this machine

Toolchain in use: Rust 1.96, Visual Studio 2022 (MSVC 19.44), CMake 4.3.4,
Ninja 1.13.2, **Qt 6.8.3 (msvc2022_64) installed at `E:\Qt\6.8.3\msvc2022_64`**.

* `cargo build` / `cargo build --release` — **OK** (Rust core staticlib produced).
* `cargo test` — **OK**, 18 tests pass (17 unit incl. FFI/file smoke + 1 integration
  FFI test driving a full Mock session over the C ABI).
* `cargo clippy --all-targets -- -D warnings` (default + `--features russh`) — **clean**.
* `cargo fmt --check` — **clean**.
* cbindgen header generation via `build.rs` — **OK**
  (`rust-core/include/research_ssh_core.h`).
* `cmake --preset windows-msvc` (VS 2022 generator, Qt 6.8.3) — **configure OK**.
* `cmake --build --preset windows-msvc --config Release` — **OK**, the full app
  (Rust core + Qt UI) compiles and links to `researchssh_next.exe`.
* Headless launch (`QT_QPA_PLATFORM=offscreen`) — **OK**: QML loads, the Rust core
  initializes, the event loop runs, no QML/type errors.
* `ctest --preset windows-msvc` — **OK** (Rust suite via CTest).

---

## Prerequisites

| Tool | Version | Check |
|------|---------|-------|
| Rust toolchain | stable (1.75+) | `cargo --version` |
| Rust MSVC target | `x86_64-pc-windows-msvc` | `rustup target list --installed` |
| Visual Studio 2022 (MSVC, C++ workload) | 2022 | `where cl` (in a Developer prompt) |
| CMake | ≥ 3.21 | `cmake --version` |
| Qt | ≥ 6.5 (MSVC x64 kit, with Qt Quick / Quick Controls) | `qmake --version` |
| Ninja (optional) | any | `ninja --version` |
| clang-format (optional, for C++ formatting) | any | `clang-format --version` |

If something is missing, install it manually:

* **Rust:** <https://rustup.rs> → `rustup default stable` →
  `rustup target add x86_64-pc-windows-msvc`
* **CMake:** <https://cmake.org/download/> (or `winget install Kitware.CMake`)
* **Qt 6:** the online installer <https://www.qt.io/download> — select a Qt 6.5+
  **MSVC 2019/2022 64-bit** kit including *Qt Quick* and *Qt Quick Controls*.

cbindgen does **not** need a global install — it runs from `build.rs` as a Cargo
build-dependency.

---

## Build & run

### 1. Rust core (works today, standalone)

```sh
cd rust-core
cargo build --release          # produces target/release/research_ssh_core.lib + header
cargo test                     # unit + FFI smoke + integration tests
cargo clippy --all-targets -- -D warnings
cargo fmt --check
```

### Real SSH (russh) build

To use real SSH (not just the Mock provider), build the Rust core with the `russh`
feature via the dedicated preset, then use the **＋ 新建连接** button in the app to
open a connection dialog (host / port / user / password). On first connect the app
shows a **host-key confirmation** dialog; accepting writes the key to
`~/.ssh/known_hosts`.

```sh
cmake --preset windows-msvc-russh -DCMAKE_PREFIX_PATH="E:/Qt/6.8.3/msvc2022_64"
cmake --build --preset windows-msvc-russh
# core only / tests:
cd rust-core && cargo test --features russh   # incl. a real in-process SSH handshake e2e test
```

The russh backend uses the pure-Rust `ring` crypto (not `aws-lc-rs`), so it needs
no extra C/asm toolchain. Password auth is wired; public-key auth is a follow-up.

### 2. Full application (Rust core + Qt UI) — requires CMake + Qt

Point CMake at your Qt kit with `CMAKE_PREFIX_PATH`, then use the preset:

```sh
# from the repository root (ResearchSSH-Next/)
cmake --preset windows-msvc -DCMAKE_PREFIX_PATH="E:/Qt/6.8.3/msvc2022_64"
cmake --build --preset windows-msvc --config Release
ctest --preset windows-msvc            # runs the Rust test suite via CTest
```

Run the app (add the Qt `bin` dir to PATH so the Qt DLLs are found, or deploy with
`windeployqt`):

```sh
export PATH="E:/Qt/6.8.3/msvc2022_64/bin:$PATH"   # PowerShell: $env:PATH = "E:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
./build/windows-msvc/Release/researchssh_next.exe
```

CMake invokes `cargo build --release` for the core automatically (see
`cmake/RustCore.cmake`) and links the resulting static library.

> **Build in Release.** The Rust core links the dynamic *release* CRT (`/MD`) on
> MSVC; building the C++ app in Debug (`/MDd`) causes a CRT mismatch. The presets
> default to Release.

> **Qt version:** the app uses `engine.loadFromModule(...)`, which needs Qt 6.5+.

### Android (reserved)

Windows is the priority for this stage; Android is scaffolded but not built here.

* Add the Rust target: `rustup target add aarch64-linux-android`.
* Build the core with the NDK toolchain (e.g. via `cargo-ndk`):
  `cargo ndk -t arm64-v8a build --release`.
* Provide Qt for Android (6.5+) and configure CMake with the Android toolchain
  file and `-DCMAKE_PREFIX_PATH=<Qt-for-Android>`. The `CredentialStore`
  abstraction already reserves the Android Keystore backend.

---

## Demo: what the Mock UI shows

1. Pick a server in the left pane, press **Connect** (or double-click a row).
2. The state badge goes **Connecting… → Connected**; a mock login banner appears
   in the center terminal.
3. Use the **research quick commands** (right pane) or type a command — the mock
   echoes it and returns canned output (`nvidia-smi`, `squeue`, `df -h`,
   `python --version`).
4. Open the **文件** tab to browse the mock remote home directory, open/edit text
   files, save them back, and exercise copy/paste/rename/delete/upload flows.
5. **Disconnect** returns to the Disconnected state.
6. The **"Unreachable (demo error)"** server (host starts with `fail`)
   demonstrates the **error** state and an error notice in the terminal.

---

## Directory responsibilities

```
ResearchSSH-Next/
├── CMakeLists.txt          # top-level build: Qt app + links the Rust core
├── CMakePresets.json       # windows-msvc (VS2022) + ninja presets
├── README.md               # this file
├── .clang-format           # C++ style (app/cpp)
├── .rustfmt.toml           # Rust style (rust-core)
├── docs/
│   └── architecture.md     # layering, ABI, ownership, threading, russh plan
├── app/                    # the C++/Qt UI layer
│   ├── cpp/
│   │   ├── main.cpp             # QGuiApplication + QML engine; exposes `app`
│   │   ├── AppController.*      # orchestration; the object QML talks to
│   │   ├── RustCoreBridge.*     # the ONLY FFI caller; marshals callbacks to UI
│   │   ├── ServerListModel.*    # QAbstractListModel for the server list
│   │   ├── TerminalViewModel.*  # terminal text buffer
│   │   ├── RemoteFileTreeModel.*# lazy remote file tree
│   │   ├── EditorViewModel.*    # in-app remote text editor state
│   │   └── CredentialStore.*    # platform secret-storage abstraction (+ Mock)
│   └── qml/
│       ├── Main.qml             # dark three-pane shell (ApplicationWindow)
│       └── components/          # ServerPane, TerminalPane, StatusPane, widgets
├── rust-core/              # the Rust security core
│   ├── Cargo.toml / Cargo.lock
│   ├── build.rs                # cbindgen: generates include/research_ssh_core.h
│   ├── cbindgen.toml
│   ├── include/research_ssh_core.h   # generated C header (committed)
│   └── src/
│       ├── lib.rs              # crate root, error codes, #![deny(unsafe_code)]
│       ├── ffi.rs              # C ABI (the only `unsafe` module)
│       ├── runtime.rs          # Tokio runtime
│       ├── session.rs          # session state machine + driver task
│       ├── event.rs            # SessionEvent + #[repr(C)] RsEvent
│       ├── provider.rs         # SshProvider trait + factory
│       ├── secret.rs           # zeroizing secret buffer
│       └── providers/
│           ├── mock.rs         # shipped Mock provider
│           └── russh.rs        # real SSH + best-effort SFTP (feature `russh`)
├── cmake/
│   └── RustCore.cmake      # builds the Rust staticlib + imported target
└── tests/
    └── CMakeLists.txt      # registers the Rust test suite with CTest
```

---

## FFI ownership rules (read before touching `RustCoreBridge`)

The C header (`rust-core/include/research_ssh_core.h`) is the contract.

1. **Handles are Rust-owned.** `RsCore*` / `RsSession*` are opaque; store the
   pointer and call the matching `*_destroy` **exactly once**. No use-after-destroy.
2. **Event pointers are transient.** `RsEvent.data` and `RsEvent.message` are valid
   **only during the callback**. Copy them immediately (the bridge copies into
   `QByteArray` / `QString`).
3. **Callbacks come from Rust threads.** Marshal onto the Qt UI thread with
   `Qt::QueuedConnection` before touching any Qt object (the bridge does this).
4. **Destroy order.** `rscore_session_destroy` joins the driver task, so after it
   returns no more callbacks fire. Destroy the session **before** the receiver
   (`AppController`). Destroy all sessions before `rscore_destroy(core)`.
5. **Input strings are borrowed.** `host` / `username` are UTF-8 and copied during
   the call; they need not outlive it.
6. **Errors** are reported as an `RsErrorCode` (or null) plus a UTF-8 message
   (static via `rscore_error_message`, per-event via `RsEvent.message`).
7. **Panics never cross.** Every entry point is wrapped in `catch_unwind`.
8. **Secrets** enter only via `rscore_session_set_password`, are copied into a
   zeroizing buffer, and never pass through QML.

---

## Next steps

The highest-value follow-ups are product hardening rather than basic wiring:

1. Replace the placeholder text terminal with a real VT/ANSI terminal grid.
2. Add public-key authentication and better SSH error reporting.
3. Add persistent server profiles and a real Windows Credential Manager backend.
4. Harden file editing for large/binary files and add operation progress/cancel.
5. Add packaging/deployment (`windeployqt`, installer) and UI tests.

---

## License

MIT OR Apache-2.0 (Rust core). Choose per your project needs before publishing.
