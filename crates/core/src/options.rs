// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Settings for contexts, sockets and servers. Defaults match the C API.

use std::time::Duration;

/// I/O thread priority, relative to the priority the thread starts with.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Priority {
    /// Leave the thread as it is.
    Normal = 0,
    /// Slightly above normal.
    Above = 1,
    /// High (the default).
    #[default]
    High = 2,
    /// Time critical.
    TimeCritical = 3,
}

impl Priority {
    /// The value used by the C API (`0..=3`).
    pub fn level(self) -> i32 {
        self as i32
    }

    /// Back from the C API value; `None` outside `0..=3`.
    pub fn from_level(level: i32) -> Option<Priority> {
        match level {
            0 => Some(Priority::Normal),
            1 => Some(Priority::Above),
            2 => Some(Priority::High),
            3 => Some(Priority::TimeCritical),
            _ => None,
        }
    }
}

/// Who may connect to a hosted server.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Permissions {
    /// Only the user that created the server.
    #[default]
    User = 0,
    /// The creating user and the local user group.
    Group = 1,
    /// Everyone on the local machine.
    World = 2,
}

/// Where the first I/O thread of a context is pinned (Windows only).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum IdealCore {
    /// Pick automatically (the default).
    #[default]
    Auto,
    /// No preference.
    Off,
    /// Start at this core.
    Core(u32),
}

/// Settings of a `Context`: its I/O threads.
#[derive(Clone, Debug)]
pub struct ContextOptions {
    /// Priority of the I/O threads. Default [`Priority::High`].
    pub priority: Priority,
    /// How long an idle I/O thread busy-polls before it blocks. Default 50 µs.
    pub spin: Duration,
    /// Number of I/O threads; 0 means one per physical core. Default 1.
    pub io_threads: usize,
    /// Windows only: core placement.
    pub ideal_core: IdealCore,
    /// Windows only: register the I/O threads with MMCSS. Default false.
    pub mmcss: bool,
    /// Windows only: alertable waits. Default false.
    pub alertable: bool,
    /// Yield the CPU after inline writes and while spinning. Default false.
    pub yield_cpu: bool,
}

impl Default for ContextOptions {
    fn default() -> Self {
        ContextOptions {
            priority: Priority::default(),
            spin: Duration::from_micros(50),
            io_threads: 1,
            ideal_core: IdealCore::default(),
            mmcss: false,
            alertable: false,
            yield_cpu: false,
        }
    }
}

/// Settings of a socket or server.
#[derive(Clone, Debug)]
pub struct Options {
    /// Who may connect; servers only. Default [`Permissions::User`].
    pub permissions: Permissions,
    /// Largest message accepted. Default 64 MiB.
    pub max_message_size: u64,
    /// Bytes that may wait in the send queue. Default 64 MiB.
    pub max_queued_bytes: u64,
    /// Pending connection slots kept ready; servers only. Default 8.
    pub backlog: usize,
    /// Maximum connected clients, 0 = unlimited; servers only. Default 0.
    pub max_clients: usize,
    /// How long connect waits for the handshake. Default 1 s.
    pub connect_timeout: Duration,
    /// Receive buffer per connection. Default 256 KiB.
    pub receive_buffer: usize,
    /// Windows kernel pipe buffer, 0 = `receive_buffer`; servers only. Default 1 MiB.
    pub pipe_buffer: usize,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            permissions: Permissions::default(),
            max_message_size: 64 << 20,
            max_queued_bytes: 64 << 20,
            backlog: 8,
            max_clients: 0,
            connect_timeout: Duration::from_millis(1000),
            receive_buffer: 256 << 10,
            pipe_buffer: 1 << 20,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_match_the_c_api() {
        let c = ContextOptions::default();
        assert_eq!(c.priority.level(), 2);
        assert_eq!(c.spin, Duration::from_micros(50));
        assert_eq!(c.io_threads, 1);
        let o = Options::default();
        assert_eq!(o.max_message_size, 64 << 20);
        assert_eq!(o.backlog, 8);
        assert_eq!(o.connect_timeout, Duration::from_millis(1000));
        assert_eq!(o.receive_buffer, 256 << 10);
        assert_eq!(o.pipe_buffer, 1 << 20);
    }

    #[test]
    fn priority_levels_round_trip() {
        for l in 0..=3 {
            assert_eq!(Priority::from_level(l).unwrap().level(), l);
        }
        assert!(Priority::from_level(4).is_none());
    }
}
