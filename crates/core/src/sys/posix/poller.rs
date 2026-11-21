// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Readiness poller: epoll on Linux and Android, woken by an eventfd.

use super::ev;
use rustix::event::epoll::{self, CreateFlags, EventData, EventFlags, EventVec};
use rustix::event::{eventfd, EventfdFlags};
use std::os::fd::{AsFd, OwnedFd};

/// Key of the wake-up eventfd; target keys are addresses and never 0.
pub(crate) const WAKE_KEY: u64 = 0;

pub(crate) struct Poller {
    epoll: OwnedFd,
    wake_fd: OwnedFd,
}

pub(crate) type Events = EventVec;

fn to_flags(events: u32) -> EventFlags {
    let mut r = EventFlags::ET;
    if events & ev::IN != 0 {
        r |= EventFlags::IN;
    }
    if events & ev::OUT != 0 {
        r |= EventFlags::OUT;
    }
    if events & ev::RDHUP != 0 {
        r |= EventFlags::RDHUP;
    }
    r
}

fn from_flags(f: EventFlags) -> u32 {
    let mut r = 0;
    if f.contains(EventFlags::IN) {
        r |= ev::IN;
    }
    if f.contains(EventFlags::OUT) {
        r |= ev::OUT;
    }
    if f.contains(EventFlags::RDHUP) {
        r |= ev::RDHUP;
    }
    if f.contains(EventFlags::HUP) {
        r |= ev::HUP;
    }
    if f.contains(EventFlags::ERR) {
        r |= ev::ERR;
    }
    r
}

impl Poller {
    pub fn new() -> std::io::Result<Poller> {
        let epoll = epoll::create(CreateFlags::CLOEXEC)?;
        let wake_fd = eventfd(0, EventfdFlags::NONBLOCK | EventfdFlags::CLOEXEC)?;
        epoll::add(
            &epoll,
            &wake_fd,
            EventData::new_u64(WAKE_KEY),
            EventFlags::IN,
        )?;
        Ok(Poller { epoll, wake_fd })
    }

    pub fn events() -> Events {
        EventVec::with_capacity(64)
    }

    /// Register `fd` (edge-triggered) for `events`.
    pub fn add(&self, fd: impl AsFd, events: u32, key: u64) -> bool {
        epoll::add(&self.epoll, fd, EventData::new_u64(key), to_flags(events)).is_ok()
    }

    /// Change the registration; this re-checks readiness, so enabling `OUT` on
    /// a socket that is already writable reports it.
    pub fn modify(&self, fd: impl AsFd, events: u32, key: u64) -> bool {
        epoll::modify(&self.epoll, fd, EventData::new_u64(key), to_flags(events)).is_ok()
    }

    pub fn remove(&self, fd: impl AsFd) {
        let _ = epoll::delete(&self.epoll, fd);
    }

    pub fn wake(&self) {
        let _ = rustix::io::write(&self.wake_fd, &1u64.to_ne_bytes());
    }

    /// Consume a wake-up.
    pub fn drain_wake(&self) {
        let mut buf = [0u8; 8];
        let _ = rustix::io::read(&self.wake_fd, &mut buf);
    }

    /// Wait up to `timeout_ms` (-1 = forever, 0 = poll) and fill `out` with
    /// (key, `ev` bits). Returns how many events there are.
    pub fn wait(&self, timeout_ms: i32, events: &mut Events, out: &mut Vec<(u64, u32)>) -> usize {
        out.clear();
        if epoll::wait(&self.epoll, events, timeout_ms).is_err() {
            return 0;
        }
        for e in events.iter() {
            out.push((e.data.u64(), from_flags(e.flags)));
        }
        out.len()
    }
}
