//! Build script: generates the C header (`include/research_ssh_core.h`) from the
//! Rust FFI surface using cbindgen.
//!
//! cbindgen runs as a *build-dependency* here, so no global `cbindgen` binary is
//! required. The generated header is committed to the repository as well, so the
//! CMake configure step can find it even before the first `cargo build`.

use std::path::PathBuf;

fn main() {
    let crate_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let include_dir = crate_dir.join("include");
    let header_path = include_dir.join("research_ssh_core.h");

    if let Err(e) = std::fs::create_dir_all(&include_dir) {
        println!("cargo:warning=could not create include dir: {e}");
        return;
    }

    let config = cbindgen::Config::from_file(crate_dir.join("cbindgen.toml"))
        .unwrap_or_else(|_| cbindgen::Config::default());

    match cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
    {
        Ok(bindings) => {
            bindings.write_to_file(&header_path);
            println!("cargo:warning=cbindgen wrote {}", header_path.display());
        }
        Err(e) => {
            // Do not fail the build: a committed header may already exist.
            println!(
                "cargo:warning=cbindgen generation failed ({e}); using committed header if present"
            );
        }
    }

    // Regenerate only when the FFI surface or config changes.
    println!("cargo:rerun-if-changed=src");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-changed=build.rs");
}
