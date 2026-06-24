# ResearchSSH-Next

一个**以 Windows 为优先目标、为 Android 预留扩展空间**的专业 SSH 客户端，面向科研和高性能计算工作流。

**架构：**稳定的 **C ABI** 连接两端：后端是负责安全边界的 **Rust 核心**（SSH/SFTP、会话、调度、异步运行时），前端是 **C++20 / Qt 6 Quick (QML)** 界面。不使用 Qt Widgets、WebView、Electron 或 Tauri。

> 项目仍处于早期产品阶段，但核心链路已经可以端到端使用。内置 **Mock provider** 可在无网络环境下驱动演示 UI；可选的 `russh` provider 支持真实 SSH 登录（公钥/密码）、主机密钥确认、PTY shell，以及尽力而为的 SFTP 文件操作。

## 当前状态

| 模块 | 状态 |
|------|------|
| Rust 核心（运行时、会话状态机、provider 抽象、事件） | 已实现 |
| C ABI（cbindgen 头文件、不透明句柄、`catch_unwind`、批量回调） | 已实现 |
| Mock SSH provider | 已实现 |
| `russh` provider（真实 SSH：握手、公钥/密码认证、主机密钥确认、PTY+shell） | 已在 `--features russh` 后实现，并通过进程内服务端端到端测试验证 |
| 连接真实服务器 UI（连接对话框 + 主机密钥确认对话框） | 已实现 |
| 远程文件管理 + 应用内编辑器（文件树 / 复制粘贴重命名 / 上传 / 编辑保存） | Mock 已实现；已贯通 FFI/C++/QML；`russh` 会在服务端支持时尝试 SFTP |
| 编辑器/终端效率功能（多文件标签、语法高亮、补全、括号配对、Python 运行目标、资源快照） | 已在 Qt UI 中实现；连接后资源刷新使用 SSH 命令 |
| Rust 单元测试 + FFI 冒烟测试 + Mock 会话测试 | 构建并通过 |
| C++/Qt 适配层（AppController、models、RustCoreBridge、CredentialStore） | 已实现 |
| QML 深色三栏界面（服务器 · 终端 · 工作区标签：连接/文件） | 已实现 |
| CMake + Cargo 构建、CMakePresets | 已实现 |
| 公钥认证（默认密钥发现、显式密钥文件、可选口令、密码回退） | 已实现 |
| 完整终端模拟器 / 端口转发 | 后续工作 |

### 本机已验证内容

当前使用的工具链：Rust 1.96、Visual Studio 2022 (MSVC 19.44)、CMake 4.3.4、Ninja 1.13.2、**Qt 6.8.3 (msvc2022_64)，安装路径为 `E:\Qt\6.8.3\msvc2022_64`**。

* `cargo build` / `cargo build --release`：通过，已生成 Rust core staticlib。
* `cargo test`：通过，18 个测试通过（17 个单元测试，包含 FFI/文件冒烟测试；1 个通过 C ABI 驱动完整 Mock 会话的集成 FFI 测试）。
* `cargo clippy --all-targets -- -D warnings`（默认特性 + `--features russh`）：无告警。
* `cargo fmt --check`：通过。
* 通过 `build.rs` 生成 cbindgen 头文件：通过（`rust-core/include/research_ssh_core.h`）。
* `cmake --preset windows-msvc`（VS 2022 generator，Qt 6.8.3）：配置通过。
* `cmake --build --preset windows-msvc --config Release`：通过，完整应用（Rust core + Qt UI）编译并链接为 `researchssh_next.exe`。
* 无头启动（`QT_QPA_PLATFORM=offscreen`）：通过，QML 加载、Rust core 初始化、事件循环运行，未出现 QML/type 错误。
* `ctest --preset windows-msvc`：通过（通过 CTest 运行 Rust 测试套件）。

## 前置条件

| 工具 | 版本 | 检查命令 |
|------|------|----------|
| Rust toolchain | stable (1.75+) | `cargo --version` |
| Rust MSVC target | `x86_64-pc-windows-msvc` | `rustup target list --installed` |
| Visual Studio 2022（MSVC、C++ 工作负载） | 2022 | 在 Developer Prompt 中运行 `where cl` |
| CMake | >= 3.21 | `cmake --version` |
| Qt | >= 6.5（MSVC x64 kit，包含 Qt Quick / Quick Controls） | `qmake --version` |
| Ninja（可选） | 任意 | `ninja --version` |
| clang-format（可选，用于 C++ 格式化） | 任意 | `clang-format --version` |

缺少依赖时可手动安装：

* **Rust：** <https://rustup.rs>，然后运行 `rustup default stable` 和 `rustup target add x86_64-pc-windows-msvc`。
* **CMake：** <https://cmake.org/download/>，或运行 `winget install Kitware.CMake`。
* **Qt 6：** 使用在线安装器 <https://www.qt.io/download>，选择 Qt 6.5+ 的 **MSVC 2019/2022 64-bit** kit，并包含 *Qt Quick* 和 *Qt Quick Controls*。

`cbindgen` 不需要全局安装；它作为 Cargo build-dependency 由 `build.rs` 运行。

## 构建与运行

### 1. Rust 核心（可独立构建）

```sh
cd rust-core
cargo build --release          # 生成 target/release/research_ssh_core.lib 和头文件
cargo test                     # 单元测试 + FFI 冒烟测试 + 集成测试
cargo clippy --all-targets -- -D warnings
cargo fmt --check
```

### 真实 SSH（russh）构建

如需使用真实 SSH（而不是 Mock provider），通过专用 preset 启用 Rust core 的 `russh` 特性，然后在应用中点击 **＋ 新建连接** 打开连接对话框（主机 / 端口 / 用户 / 密码 / 可选私钥）。私钥路径留空时会按顺序自动发现 `~/.ssh/id_ed25519`、`id_ecdsa`、`id_rsa`；如果提供了密码，会在公钥认证失败后作为回退方式使用。首次连接时，应用会显示**主机密钥确认**对话框；接受后会把密钥写入 `~/.ssh/known_hosts`。

```sh
cmake --preset windows-msvc-russh -DCMAKE_PREFIX_PATH="E:/Qt/6.8.3/msvc2022_64"
cmake --build --preset windows-msvc-russh
# 只构建/测试核心：
cd rust-core && cargo test --features russh   # 包含公钥与密码 loopback 端到端测试
```

`russh` 后端使用纯 Rust 的 `ring` 加密后端（不使用 `aws-lc-rs`），因此不需要额外的 C/汇编工具链。认证顺序是先尝试公钥，密码认证仍作为回退可用。

### 2. 完整应用（Rust core + Qt UI）

完整应用需要 CMake 和 Qt。先通过 `CMAKE_PREFIX_PATH` 指向本机 Qt kit，再使用 preset：

```sh
# 在仓库根目录（ResearchSSH-Next/）执行
cmake --preset windows-msvc -DCMAKE_PREFIX_PATH="E:/Qt/6.8.3/msvc2022_64"
cmake --build --preset windows-msvc --config Release
ctest --preset windows-msvc            # 通过 CTest 运行 Rust 测试套件
```

运行应用时，需要让 Qt DLL 可被找到：把 Qt `bin` 目录加入 PATH，或使用 `windeployqt` 部署。

```sh
export PATH="E:/Qt/6.8.3/msvc2022_64/bin:$PATH"   # PowerShell: $env:PATH = "E:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
./build/windows-msvc/Release/researchssh_next.exe
```

CMake 会自动调用 `cargo build --release` 构建核心库（见 `cmake/RustCore.cmake`），并链接生成的静态库。

> **请使用 Release 构建。** Rust core 在 MSVC 上链接动态 release CRT（`/MD`）；如果 C++ 应用使用 Debug（`/MDd`），会出现 CRT 不匹配。项目 preset 默认使用 Release。

> **Qt 版本：**应用使用 `engine.loadFromModule(...)`，需要 Qt 6.5+。

### Android（预留）

当前阶段优先 Windows；Android 已预留结构，但本仓库尚未在 Android 上完成构建验证。

* 添加 Rust target：`rustup target add aarch64-linux-android`。
* 使用 NDK 工具链构建核心，例如通过 `cargo-ndk`：`cargo ndk -t arm64-v8a build --release`。
* 准备 Qt for Android（6.5+），并使用 Android toolchain file 与 `-DCMAKE_PREFIX_PATH=<Qt-for-Android>` 配置 CMake。`CredentialStore` 抽象已经预留 Android Keystore 后端。

## Mock UI 演示流程

1. 在左侧选择一个服务器，点击 **Connect**，或双击某一行。
2. 状态徽标依次变为 **Connecting... -> Connected**；中间终端会显示 mock 登录欢迎信息。
3. 使用右侧的 **research quick commands**，或直接输入命令；mock 会回显命令并返回预设输出（`nvidia-smi`、`squeue`、`df -h`、`python --version`）。
4. 打开 **文件** 标签页，可以浏览 mock 远程 home 目录，打开/编辑文本文件并保存，也可以验证复制、粘贴、重命名、删除、上传流程。
5. 点击 **Disconnect** 返回 Disconnected 状态。
6. **"Unreachable (demo error)"** 服务器（host 以 `fail` 开头）用于演示 **error** 状态和终端错误提示。

## 目录职责

```text
ResearchSSH-Next/
├── CMakeLists.txt          # 顶层构建：Qt 应用 + 链接 Rust core
├── CMakePresets.json       # windows-msvc（VS2022）+ ninja preset
├── README.md               # 本文件
├── .clang-format           # C++ 代码风格（app/cpp）
├── .rustfmt.toml           # Rust 代码风格（rust-core）
├── docs/
│   └── architecture.md     # 分层、ABI、所有权、线程、russh 计划
├── app/                    # C++/Qt UI 层
│   ├── cpp/
│   │   ├── main.cpp             # QGuiApplication + QML engine；暴露 `app`
│   │   ├── AppController.*      # 编排层；QML 交互的对象
│   │   ├── RustCoreBridge.*     # 唯一调用 FFI 的位置；把回调转发到 UI 线程
│   │   ├── ServerListModel.*    # 服务器列表的 QAbstractListModel
│   │   ├── TerminalViewModel.*  # 终端文本缓冲区
│   │   ├── RemoteFileTreeModel.*# 懒加载远程文件树
│   │   ├── EditorViewModel.*    # 应用内远程文本编辑器状态
│   │   └── CredentialStore.*    # 平台密钥存储抽象（含 Mock）
│   └── qml/
│       ├── Main.qml             # 深色三栏外壳（ApplicationWindow）
│       └── components/          # ServerPane、TerminalPane、StatusPane 等组件
├── rust-core/              # Rust 安全核心
│   ├── Cargo.toml / Cargo.lock
│   ├── build.rs                # cbindgen：生成 include/research_ssh_core.h
│   ├── cbindgen.toml
│   ├── include/research_ssh_core.h   # 已提交的生成 C 头文件
│   └── src/
│       ├── lib.rs              # crate root、错误码、#![deny(unsafe_code)]
│       ├── ffi.rs              # C ABI（唯一允许 `unsafe` 的模块）
│       ├── runtime.rs          # Tokio runtime
│       ├── session.rs          # 会话状态机 + driver task
│       ├── event.rs            # SessionEvent + #[repr(C)] RsEvent
│       ├── provider.rs         # SshProvider trait + factory
│       ├── secret.rs           # zeroizing secret buffer
│       └── providers/
│           ├── mock.rs         # 内置 Mock provider
│           └── russh.rs        # 真实 SSH + 尽力 SFTP（feature `russh`）
├── cmake/
│   └── RustCore.cmake      # 构建 Rust staticlib + imported target
└── tests/
    └── CMakeLists.txt      # 把 Rust 测试套件注册到 CTest
```

## FFI 所有权规则

修改 `RustCoreBridge` 前请先阅读这一节。C 头文件 `rust-core/include/research_ssh_core.h` 是跨语言边界的契约。

1. **句柄归 Rust 所有。** `RsCore*` / `RsSession*` 是不透明指针；C++ 只保存指针，并且必须对每个句柄**恰好调用一次**对应的 `*_destroy`。禁止 destroy 后继续使用。
2. **事件指针是临时借用。** `RsEvent.data` 和 `RsEvent.message` 只在回调期间有效。必须立刻复制出来（bridge 会复制到 `QByteArray` / `QString`）。
3. **回调来自 Rust 线程。** 触碰任何 Qt 对象前，必须用 `Qt::QueuedConnection` 切回 Qt UI 线程（bridge 已处理）。
4. **销毁顺序。** `rscore_session_destroy` 会 join driver task，因此它返回后不会再触发回调。先销毁 session，再销毁接收方（`AppController`）。所有 session 都销毁后再调用 `rscore_destroy(core)`。
5. **输入字符串是借用的。** `host` / `username` 是 UTF-8，并会在调用期间被 Rust 复制；它们不需要在调用后继续存活。
6. **错误**通过 `RsErrorCode`（或空指针）和 UTF-8 消息报告：静态消息来自 `rscore_error_message`，事件级消息来自 `RsEvent.message`。
7. **panic 不跨边界。** 所有入口都包在 `catch_unwind` 内。
8. **密钥材料**只通过 `rscore_session_set_password` 等 secret 通道进入核心，会被复制进 zeroizing buffer，绝不经过 QML。

## 后续工作

当前最高价值的后续工作是产品硬化，而不是基础链路搭建：

1. 用真正的 VT/ANSI 终端网格替换当前占位文本终端。
2. 改进 SSH 错误报告，并增加 keyboard-interactive / agent 认证。
3. 增加持久化服务器配置，以及真正的 Windows Credential Manager 后端。
4. 加固文件编辑：支持大文件/二进制文件处理、进度显示和取消操作。
5. 增加打包部署（`windeployqt`、安装器）和 UI 测试。

## License

MIT OR Apache-2.0（Rust core）。发布前请按项目需要确定最终授权策略。
