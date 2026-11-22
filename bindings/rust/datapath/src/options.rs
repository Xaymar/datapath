// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

use core::mem::MaybeUninit;
use std::time::Duration;

/// I/O thread priority.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Priority {
    Normal,
    AboveNormal,
    Highest,
    TimeCritical,
}

/// Windows only: how I/O threads are steered to physical cores.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum IdealCore {
    /// Start at a core derived from the process id, skipping core 0 when there
    /// are more than two cores.
    Auto,
    /// Leave placement to the OS.
    Off,
    /// Thread `i` prefers physical core `n + i`.
    First(u32),
}

/// Who may connect to a hosted server.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Permissions {
    /// Only the user that created the server.
    User,
    /// The creating user and the local user group.
    Group,
    /// Everyone on the local machine.
    World,
}

/// Settings shared by every socket and server on one [`Context`](crate::Context).
///
/// `Default` returns the library's own defaults. See `include/datapath/options.hpp`
/// for the measurements behind each one.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub struct ContextOptions {
    /// How long an idle I/O thread busy-polls before blocking. Zero disables it.
    pub spin: Duration,
    /// Number of I/O threads; 0 = one per physical core.
    pub io_threads: usize,
    pub priority: Priority,
    /// Windows only.
    pub ideal_core: IdealCore,
    /// Windows only: register I/O threads with MMCSS.
    pub mmcss: bool,
    /// Windows only: alertable waits.
    pub alertable: bool,
    /// Yield the CPU after inline writes and while spinning.
    pub yield_cpu: bool,
    /// Windows only: pair with a peer that also sets this on the two
    /// hyper-threads of one physical core (needs `io_threads` 1).
    pub pair_peer: bool,
}

impl Default for ContextOptions {
    fn default() -> Self {
        let mut raw = MaybeUninit::<sys::datapath_context_options>::zeroed();
        // SAFETY: init fills the whole struct.
        let raw = unsafe {
            sys::datapath_context_options_init(raw.as_mut_ptr());
            raw.assume_init()
        };
        ContextOptions {
            spin: Duration::from_micros(raw.spin_us.max(0) as u64),
            io_threads: raw.io_threads as usize,
            priority: match raw.priority {
                0 => Priority::Normal,
                1 => Priority::AboveNormal,
                3 => Priority::TimeCritical,
                _ => Priority::Highest,
            },
            ideal_core: match raw.ideal_core {
                -2 => IdealCore::Off,
                n if n >= 0 => IdealCore::First(n as u32),
                _ => IdealCore::Auto,
            },
            mmcss: raw.mmcss != 0,
            alertable: raw.alertable != 0,
            yield_cpu: raw.yield_ != 0,
            pair_peer: raw.pair_peer != 0,
        }
    }
}

impl ContextOptions {
    pub(crate) fn to_raw(&self) -> sys::datapath_context_options {
        let mut raw = MaybeUninit::<sys::datapath_context_options>::zeroed();
        // SAFETY: init fills the whole struct, including struct_size.
        let mut raw = unsafe {
            sys::datapath_context_options_init(raw.as_mut_ptr());
            raw.assume_init()
        };
        raw.spin_us = i64::try_from(self.spin.as_micros()).unwrap_or(i64::MAX);
        raw.io_threads = self.io_threads as u64;
        raw.priority = match self.priority {
            Priority::Normal => 0,
            Priority::AboveNormal => 1,
            Priority::Highest => 2,
            Priority::TimeCritical => 3,
        };
        raw.ideal_core = match self.ideal_core {
            IdealCore::Auto => -1,
            IdealCore::Off => -2,
            IdealCore::First(n) => i32::try_from(n).unwrap_or(i32::MAX),
        };
        raw.mmcss = self.mmcss as u8;
        raw.alertable = self.alertable as u8;
        raw.yield_ = self.yield_cpu as u8;
        raw.pair_peer = self.pair_peer as u8;
        raw
    }
}

/// Per-connection and per-server settings.
///
/// `Default` returns the library's own defaults.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub struct Options {
    /// Largest message accepted. A connection uses the smaller of both peers' values.
    pub max_message_size: u64,
    /// Bytes that may wait in the send queue before `send` returns `WouldBlock`.
    pub max_queued_bytes: u64,
    /// Who may connect (host only).
    pub permissions: Permissions,
    /// Pending connection slots kept ready (host only).
    pub backlog: usize,
    /// Maximum simultaneously connected clients, 0 = unlimited (host only).
    pub max_clients: usize,
    /// How long connect waits for the server's handshake (connect only).
    pub connect_timeout: Duration,
    /// Receive buffer per connection.
    pub receive_buffer: usize,
    /// Windows kernel pipe buffer, 0 = same as `receive_buffer` (host only).
    pub pipe_buffer: usize,
}

impl Default for Options {
    fn default() -> Self {
        let mut raw = MaybeUninit::<sys::datapath_options>::zeroed();
        // SAFETY: init fills the whole struct.
        let raw = unsafe {
            sys::datapath_options_init(raw.as_mut_ptr());
            raw.assume_init()
        };
        Options {
            max_message_size: raw.max_message_size,
            max_queued_bytes: raw.max_queued_bytes,
            permissions: match raw.permissions {
                sys::DATAPATH_PERMISSIONS_GROUP => Permissions::Group,
                sys::DATAPATH_PERMISSIONS_WORLD => Permissions::World,
                _ => Permissions::User,
            },
            backlog: raw.backlog as usize,
            max_clients: raw.max_clients as usize,
            connect_timeout: Duration::from_millis(raw.connect_timeout_ms.max(0) as u64),
            receive_buffer: raw.receive_buffer as usize,
            pipe_buffer: raw.pipe_buffer as usize,
        }
    }
}

impl Options {
    pub(crate) fn to_raw(&self) -> sys::datapath_options {
        let mut raw = MaybeUninit::<sys::datapath_options>::zeroed();
        // SAFETY: init fills the whole struct, including struct_size.
        let mut raw = unsafe {
            sys::datapath_options_init(raw.as_mut_ptr());
            raw.assume_init()
        };
        raw.max_message_size = self.max_message_size;
        raw.max_queued_bytes = self.max_queued_bytes;
        raw.permissions = match self.permissions {
            Permissions::User => sys::DATAPATH_PERMISSIONS_USER,
            Permissions::Group => sys::DATAPATH_PERMISSIONS_GROUP,
            Permissions::World => sys::DATAPATH_PERMISSIONS_WORLD,
        };
        raw.backlog = self.backlog as u64;
        raw.max_clients = self.max_clients as u64;
        raw.connect_timeout_ms = i64::try_from(self.connect_timeout.as_millis()).unwrap_or(i64::MAX);
        raw.receive_buffer = self.receive_buffer as u64;
        raw.pipe_buffer = self.pipe_buffer as u64;
        raw
    }
}
