// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Readiness poller: epoll on Linux and Android, kqueue on macOS. Both expose
//! the same small interface: edge-triggered registrations keyed by a `u64`, a
//! wake-up that can be signalled from any thread, and a wait that yields
//! (key, `ev` bits) pairs.

#[cfg(any(target_os = "linux", target_os = "android"))]
mod epoll;
#[cfg(any(target_os = "linux", target_os = "android"))]
pub(crate) use epoll::*;

#[cfg(target_os = "macos")]
mod kqueue;
#[cfg(target_os = "macos")]
pub(crate) use kqueue::*;
