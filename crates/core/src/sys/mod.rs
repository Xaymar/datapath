// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Operating-system backends. Each one provides the same `Context`, `Conn`
//! and `Listener` types to the public API.

#[cfg(any(target_os = "linux", target_os = "android", target_os = "macos"))]
mod posix;
#[cfg(any(target_os = "linux", target_os = "android", target_os = "macos"))]
pub(crate) use posix::*;

#[cfg(windows)]
mod windows;
#[cfg(windows)]
pub(crate) use windows::*;
