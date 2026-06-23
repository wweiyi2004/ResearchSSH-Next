# CTest integration.
#
# The substantive tests live in the Rust core (unit tests + an FFI smoke test +
# an integration-level FFI test that drives a full Mock session over the C ABI).
# We register them with CTest so `ctest` runs the whole suite. C++/QML test
# targets can be added here later (e.g. Qt Test / QML TestCase).

add_test(
    NAME rust_core_tests
    COMMAND ${CARGO_EXECUTABLE} test
            --manifest-path "${CMAKE_SOURCE_DIR}/rust-core/Cargo.toml"
            --target-dir "${RUST_TARGET_DIR}")

set_tests_properties(rust_core_tests PROPERTIES
    LABELS "rust;ffi"
    TIMEOUT 300)
