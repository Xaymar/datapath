// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! kqueue, woken by an `EVFILT_USER` event (macOS).
//!
//! `ev::IN` and `ev::OUT` map to a read and a write filter, added with
//! `EV_CLEAR` (edge triggered, like `EPOLLET`) while their bit is set and
//! deleted when it is cleared. A newly added filter is evaluated at once, so a
//! socket that is already writable reports `ev::OUT` as soon as it is enabled.

// `rustix::event::kqueue::kevent` is unsafe because its change list may carry
// pointers in `udata`. Here `udata` is only ever an integer key that is never
// dereferenced, and file descriptors are removed from the kqueue before they are
// closed or are dropped from it by the kernel when closed.
#![allow(unsafe_code)]

use crate::sys::posix::ev;
use rustix::event::kqueue::{kevent, kqueue, Event, EventFilter, EventFlags, UserFlags};
use std::os::fd::{AsFd, AsRawFd, OwnedFd};
use std::time::Duration;

/// Key of the wake-up event; target keys are addresses and never 0.
pub(crate) const WAKE_KEY: u64 = 0;

/// Identifier of the `EVFILT_USER` event.
const WAKE_IDENT: isize = 1;

pub(crate) struct Poller {
    kq: OwnedFd,
}

pub(crate) type Events = Vec<Event>;

fn apply(kq: &OwnedFd, changes: &[Event]) -> bool {
    if changes.is_empty() {
        return true;
    }
    let mut none = Vec::new();
    // SAFETY: see the module comment.
    unsafe { kevent(kq, changes, &mut none, None) }.is_ok()
}

/// The changes that take `fd` from `old` to `new` events.
fn filter_changes(fd: i32, old: u32, new: u32, key: u64) -> Vec<Event> {
    let mut changes = Vec::with_capacity(2);
    for (bit, read) in [(ev::IN, true), (ev::OUT, false)] {
        let was = old & bit != 0;
        let is = new & bit != 0;
        if was == is {
            continue;
        }
        let filter = if read {
            EventFilter::Read(fd)
        } else {
            EventFilter::Write(fd)
        };
        let flags = if is {
            EventFlags::ADD | EventFlags::CLEAR
        } else {
            EventFlags::DELETE
        };
        changes.push(Event::new(filter, flags, key as isize));
    }
    changes
}

impl Poller {
    pub fn new() -> std::io::Result<Poller> {
        let kq = kqueue()?;
        rustix::io::fcntl_setfd(&kq, rustix::io::FdFlags::CLOEXEC)?;
        let wake = Event::new(
            EventFilter::User {
                ident: WAKE_IDENT,
                flags: UserFlags::empty(),
                user_flags: rustix::event::kqueue::UserDefinedFlags::new(0),
            },
            EventFlags::ADD | EventFlags::CLEAR,
            0,
        );
        if !apply(&kq, &[wake]) {
            return Err(std::io::Error::last_os_error());
        }
        Ok(Poller { kq })
    }

    pub fn events() -> Events {
        Vec::with_capacity(64)
    }

    /// Register `fd` (edge-triggered) for `events`.
    pub fn add(&self, fd: impl AsFd, events: u32, key: u64) -> bool {
        let fd = fd.as_fd().as_raw_fd();
        if apply(&self.kq, &filter_changes(fd, 0, events, key)) {
            true
        } else {
            self.remove_raw(fd);
            false
        }
    }

    /// Change the registration from `old_events` to `events`.
    pub fn modify(&self, fd: impl AsFd, old_events: u32, events: u32, key: u64) -> bool {
        apply(
            &self.kq,
            &filter_changes(fd.as_fd().as_raw_fd(), old_events, events, key),
        )
    }

    pub fn remove(&self, fd: impl AsFd) {
        self.remove_raw(fd.as_fd().as_raw_fd());
    }

    fn remove_raw(&self, fd: i32) {
        // EV_RECEIPT reports each change separately, so a missing filter does not
        // stop the other one from being deleted.
        let flags = EventFlags::DELETE | EventFlags::RECEIPT;
        let changes = [
            Event::new(EventFilter::Read(fd), flags, 0),
            Event::new(EventFilter::Write(fd), flags, 0),
        ];
        let mut results = Vec::with_capacity(2);
        // SAFETY: see the module comment.
        let _ = unsafe { kevent(&self.kq, &changes, &mut results, Some(Duration::ZERO)) };
    }

    pub fn wake(&self) {
        let trigger = Event::new(
            EventFilter::User {
                ident: WAKE_IDENT,
                flags: UserFlags::TRIGGER,
                user_flags: rustix::event::kqueue::UserDefinedFlags::new(0),
            },
            EventFlags::empty(),
            0,
        );
        let _ = apply(&self.kq, &[trigger]);
    }

    /// The user event is `EV_CLEAR`: it resets when delivered.
    pub fn drain_wake(&self) {}

    /// Wait up to `timeout_ms` (-1 = forever, 0 = poll) and fill `out` with
    /// (key, `ev` bits). Returns how many events there are.
    pub fn wait(&self, timeout_ms: i32, events: &mut Events, out: &mut Vec<(u64, u32)>) -> usize {
        out.clear();
        let timeout = (timeout_ms >= 0).then(|| Duration::from_millis(timeout_ms as u64));
        // SAFETY: see the module comment.
        if unsafe { kevent(&self.kq, &[], events, timeout) }.is_err() {
            return 0;
        }
        for e in events.iter() {
            if let EventFilter::User { .. } = e.filter() {
                out.push((WAKE_KEY, 0));
                continue;
            }
            let key = e.udata() as u64;
            if key == 0 {
                continue;
            }
            let flags = e.flags();
            let eof = flags.contains(EventFlags::EOF);
            let bits = if flags.contains(EventFlags::ERROR) {
                ev::ERR
            } else {
                match e.filter() {
                    EventFilter::Read(_) => ev::IN | if eof { ev::RDHUP } else { 0 },
                    EventFilter::Write(_) => ev::OUT | if eof { ev::HUP } else { 0 },
                    _ => continue,
                }
            };
            out.push((key, bits));
        }
        out.len()
    }
}
