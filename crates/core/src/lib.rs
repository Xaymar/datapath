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
//!
//! None of these modules does I/O or needs `unsafe`.

#![deny(unsafe_code)]
#![warn(missing_docs)]

pub mod error;
pub mod proto;
pub mod queue;
pub mod spin;

pub use error::Error;
