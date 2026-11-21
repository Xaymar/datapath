// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! The C ABI of DataPath, implemented over the native Rust core.
//!
//! The functions, structs and constants here are the ones declared in
//! `include/datapath/datapath.h` (C ABI version 1), which stays the header of
//! record: a binary built against it links against this library unchanged.
//! The conventions are those of that header: opaque handles, error codes with
//! out-parameters left untouched on failure, callbacks that receive a `user`
//! pointer and an optional `user_free`, size-tagged option structs.

#![allow(non_camel_case_types)]
#![allow(clippy::missing_safety_doc)]

#[cfg(any(target_os = "linux", target_os = "android", target_os = "macos"))]
mod imp;

#[cfg(any(target_os = "linux", target_os = "android", target_os = "macos"))]
pub use imp::*;
