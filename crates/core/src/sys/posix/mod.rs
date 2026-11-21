// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! POSIX backend: AF_UNIX stream sockets, one readiness poller (epoll on
//! Linux and Android, kqueue on the BSDs and macOS) per I/O thread.

mod conn;
mod context;
mod evloop;
mod listener;
mod path;
mod platform;
mod poller;

pub(crate) use conn::UdsConn as Conn;
pub(crate) use context::{ContextHandle, ContextShared};
pub(crate) use listener::Listener;

/// Readiness bits, independent of the poller.
pub(crate) mod ev {
    pub const IN: u32 = 1 << 0;
    pub const OUT: u32 = 1 << 1;
    pub const RDHUP: u32 = 1 << 2;
    pub const HUP: u32 = 1 << 3;
    pub const ERR: u32 = 1 << 4;
}

/// Anything registered with an event loop.
pub(crate) trait Target: Send + Sync {
    /// Readiness (`ev` bits) of the target's fd. Runs on its loop thread.
    fn on_event(&self, events: u32);
    /// A posted notification. Runs on its loop thread.
    fn on_post(&self, kind: i32);
}

/// Registration key of a target: its address.
pub(crate) fn key_of<T: ?Sized>(a: &std::sync::Arc<T>) -> u64 {
    std::sync::Arc::as_ptr(a) as *const () as usize as u64
}
