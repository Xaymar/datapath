// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Compiles `c/smoke.c` against the real `include/datapath/datapath.h` and links
//! it with this crate's static library: the header stays the contract, and this
//! proves the Rust library fulfils it.

#![cfg(any(target_os = "linux", target_os = "android", target_os = "macos"))]

use std::path::PathBuf;
use std::process::Command;

fn run(cmd: &mut Command) -> Option<std::process::Output> {
    match cmd.output() {
        Ok(o) => Some(o),
        Err(e) => {
            eprintln!("skipping: cannot run {:?}: {e}", cmd.get_program());
            None
        }
    }
}

#[test]
fn c_program_against_the_real_header_works() {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let root = manifest.join("../..");
    let target = manifest.join("../target");

    // Build the static library with the profile this test runs under.
    let profile = if cfg!(debug_assertions) {
        "debug"
    } else {
        "release"
    };
    let mut build = Command::new(env!("CARGO"));
    build.args(["build", "-p", "datapath-capi", "--manifest-path"]);
    build.arg(manifest.join("../Cargo.toml"));
    if profile == "release" {
        build.arg("--release");
    }
    let out = run(&mut build).expect("cargo");
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let lib = target.join(profile).join("libdatapath_capi.a");
    assert!(lib.exists(), "{} missing", lib.display());

    // The header includes <datapath/export.h>, which CMake generates; a stub is
    // enough for declarations.
    let dir = std::env::temp_dir().join(format!("datapath-capi-abi-{}", std::process::id()));
    std::fs::create_dir_all(dir.join("datapath")).unwrap();
    std::fs::write(dir.join("datapath/export.h"), "#define DATAPATH_API\n").unwrap();
    let exe = dir.join("smoke");

    let mut cc = Command::new(std::env::var("CC").unwrap_or_else(|_| "cc".into()));
    cc.arg("-std=c11")
        .arg("-D_GNU_SOURCE")
        .arg("-Wall")
        .arg("-I")
        .arg(&dir)
        .arg("-I")
        .arg(root.join("include"))
        .arg(manifest.join("tests/c/smoke.c"))
        .arg(&lib)
        .args(["-lpthread", "-ldl", "-lm", "-o"])
        .arg(&exe);
    let Some(out) = run(&mut cc) else { return };
    assert!(
        out.status.success(),
        "C compile failed:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );

    let out = run(&mut Command::new(&exe)).unwrap();
    let _ = std::fs::remove_dir_all(&dir);
    assert!(
        out.status.success(),
        "C smoke test failed:\n{}{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
}
