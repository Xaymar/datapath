// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Native Rust core of DataPath.
//!
//! This crate is being built up in phases (see the migration plan): the
//! platform-free parts come first, then the POSIX and Windows backends, then the
//! C ABI and the language bindings. Until the switch-over it lives beside the
//! C++ core and is not published.
//!
//! * [`error`]: the error codes shared with the C API.
//! * [`proto`]: the wire format (frame header, handshake, incremental frame parser).
//! * [`queue`]: the bounded, coalescing, multi-producer send queue.
//! * [`spin`]: busy-poll for a short budget, then block.
//! * [`options`], [`conn`]: settings and the platform-neutral connection state machine.
//! * [`api`]: contexts, sockets and servers, on Linux, Android and macOS so far.
//!
//! The platform-free modules do no I/O and need no `unsafe`. The backend under
//! `sys` is built on `rustix` and is safe code too, except for three small
//! spots on macOS where `rustix` has no wrapper (kqueue's `kevent`, the
//! quality-of-service call, and `sysctlbyname`), each a single FFI call.

#![deny(unsafe_code)]
#![warn(missing_docs)]

// The connection core is only used by a backend; until Windows has one, its
// internals are unused there.
#[cfg_attr(
    not(any(target_os = "linux", target_os = "android", target_os = "macos")),
    allow(dead_code)
)]
pub mod conn;
pub mod error;
pub mod options;
pub mod proto;
pub mod queue;
pub mod spin;

#[cfg(any(target_os = "linux", target_os = "android", target_os = "macos"))]
pub mod api;
#[cfg(any(target_os = "linux", target_os = "android", target_os = "macos"))]
mod sys;

#[cfg(any(target_os = "linux", target_os = "android", target_os = "macos"))]
pub use api::{
    ConnectBuilder, Context, ContextBuilder, HostBuilder, Incoming, PendingAccept, Sender, Server,
    Socket,
};
pub use conn::{CloseReason, SendError};
pub use error::Error;
pub use options::{ContextOptions, IdealCore, Options, Permissions, Priority};
