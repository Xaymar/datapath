// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause


//! Builds the DataPath C++ library with CMake and links it statically.
//!
//! Environment variables:
//! - `DATAPATH_LIB_DIR`: link a prebuilt DataPath from this directory instead
//!   of building it. `DATAPATH_LINK` then picks `dylib` (default) or `static`.
//! - `DATAPATH_SOURCE_DIR`: build from this source tree instead of the
//!   vendored copy or the enclosing repository.

use std::env;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=DATAPATH_LIB_DIR");
    println!("cargo:rerun-if-env-changed=DATAPATH_LINK");
    println!("cargo:rerun-if-env-changed=DATAPATH_SOURCE_DIR");

    // docs.rs only needs the Rust declarations.
    if env::var_os("DOCS_RS").is_some() {
        return;
    }

    if let Some(dir) = env::var_os("DATAPATH_LIB_DIR") {
        println!("cargo:rustc-link-search=native={}", PathBuf::from(dir).display());
        let kind = env::var("DATAPATH_LINK").unwrap_or_else(|_| "dylib".into());
        match kind.as_str() {
            "static" => {
                println!("cargo:rustc-link-lib=static=datapath");
                link_runtime();
            }
            "dylib" => println!("cargo:rustc-link-lib=dylib=datapath"),
            other => panic!("DATAPATH_LINK must be `static` or `dylib`, not `{other}`"),
        }
        return;
    }

    let source = source_dir();
    for sub in ["CMakeLists.txt", "cmake", "include", "source"] {
        println!("cargo:rerun-if-changed={}", source.join(sub).display());
    }

    // Always an optimized build: this is a latency library, and a debug build
    // of the C++ side says nothing useful about it.
    let dst = cmake::Config::new(&source)
        .profile("Release")
        .define("DATAPATH_SHARED", "OFF")
        .define("DATAPATH_BUILD_TESTS", "OFF")
        .define("DATAPATH_BUILD_SAMPLES", "OFF")
        .define("CMAKE_INSTALL_LIBDIR", "lib")
        .build();

    println!("cargo:rustc-link-search=native={}", dst.join("lib").display());
    println!("cargo:rustc-link-lib=static=datapath");
    println!("cargo:include={}", dst.join("include").display());
    link_runtime();
}

/// Where the C++ sources are: an explicit override, the copy vendored into the
/// published crate, or the repository this crate lives in.
fn source_dir() -> PathBuf {
    if let Some(dir) = env::var_os("DATAPATH_SOURCE_DIR") {
        return PathBuf::from(dir);
    }
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let vendored = manifest.join("vendor");
    if is_source_tree(&vendored) {
        return vendored;
    }
    let repo = manifest.join("../../..");
    if is_source_tree(&repo) {
        return repo;
    }
    panic!(
        "DataPath sources not found. Set DATAPATH_SOURCE_DIR to a DataPath checkout, \
         or DATAPATH_LIB_DIR to a prebuilt library."
    );
}

fn is_source_tree(dir: &Path) -> bool {
    dir.join("CMakeLists.txt").is_file() && dir.join("include/datapath/datapath.h").is_file()
}

/// The static library is C++: link its runtime and system dependencies.
fn link_runtime() {
    let os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let target_env = env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();
    match (os.as_str(), target_env.as_str()) {
        ("windows", "msvc") => {}
        ("windows", _) => println!("cargo:rustc-link-lib=stdc++"),
        ("macos" | "ios", _) => println!("cargo:rustc-link-lib=c++"),
        ("android", _) => println!("cargo:rustc-link-lib=c++_shared"),
        _ => println!("cargo:rustc-link-lib=stdc++"),
    }
    if os == "windows" {
        println!("cargo:rustc-link-lib=advapi32");
        println!("cargo:rustc-link-lib=kernel32");
    }
}
