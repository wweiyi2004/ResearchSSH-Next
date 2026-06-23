# RustCore.cmake
#
# Builds the Rust security core (research_ssh_core) as a static library via Cargo
# and exposes it as the imported target `researchssh_core`.
#
# Design notes:
#   * The Rust core is ALWAYS built in release (`cargo build --release`). Rust on
#     the MSVC target links the dynamic release CRT (/MD); building the C++/Qt app
#     in Release (also /MD) keeps the CRTs consistent. Building the app in Debug
#     (/MDd) against this lib will cause CRT mismatch — configure Release.
#   * build.rs regenerates rust-core/include/research_ssh_core.h (via cbindgen)
#     during the cargo build, so the header always matches the FFI surface.

find_program(CARGO_EXECUTABLE cargo)
if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR
        "cargo (Rust toolchain) was not found on PATH.\n"
        "Install Rust via https://rustup.rs and ensure the MSVC target is present:\n"
        "    rustup default stable\n"
        "    rustup target add x86_64-pc-windows-msvc\n"
        "Then re-run CMake.")
endif()

set(RUST_CORE_DIR "${CMAKE_SOURCE_DIR}/rust-core")
set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-target")
set(RUST_CORE_LIB "${RUST_TARGET_DIR}/release/research_ssh_core.lib")
set(RUST_CORE_HEADER_DIR "${RUST_CORE_DIR}/include")

if(NOT EXISTS "${RUST_CORE_HEADER_DIR}")
    file(MAKE_DIRECTORY "${RUST_CORE_HEADER_DIR}")
endif()

# Cargo features to forward (e.g. -DRESEARCHSSH_RUST_FEATURES=russh).
set(RESEARCHSSH_RUST_FEATURES "" CACHE STRING "Extra cargo features for the Rust core")
set(_cargo_feature_args "")
if(RESEARCHSSH_RUST_FEATURES)
    set(_cargo_feature_args --features "${RESEARCHSSH_RUST_FEATURES}")
endif()

# Invoke cargo on every build. CMake cannot track the Rust source graph, so rather
# than risk linking a stale static library we always run cargo; its own incremental
# compilation makes this a fast no-op when no Rust sources changed. (A custom target
# — unlike a custom command with an OUTPUT — is always considered out of date.)
add_custom_target(rust_core_build
    BYPRODUCTS "${RUST_CORE_LIB}"
    COMMAND ${CARGO_EXECUTABLE} build --release
            --manifest-path "${RUST_CORE_DIR}/Cargo.toml"
            --target-dir "${RUST_TARGET_DIR}"
            ${_cargo_feature_args}
    WORKING_DIRECTORY "${RUST_CORE_DIR}"
    COMMENT "Building Rust core (cargo build --release)"
    VERBATIM
    USES_TERMINAL)

add_library(researchssh_core STATIC IMPORTED GLOBAL)
add_dependencies(researchssh_core rust_core_build)

# The lib may not exist at configure time (first build); the dependency above
# guarantees cargo runs before anything links against it.
set_target_properties(researchssh_core PROPERTIES
    IMPORTED_LOCATION "${RUST_CORE_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${RUST_CORE_HEADER_DIR}")

# System libraries required to link the Rust std + Tokio static library on the
# Windows MSVC target. If the linker reports unresolved externals, regenerate the
# exact list with:
#   cargo rustc --release -- --print native-static-libs
if(WIN32)
    set_target_properties(researchssh_core PROPERTIES
        INTERFACE_LINK_LIBRARIES
            "kernel32;advapi32;bcrypt;ntdll;userenv;ws2_32")
endif()
