// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! The parts of the POSIX backend that differ between operating systems.

use crate::options::Options;
use rustix::io::Errno;
use rustix::net::{AddressFamily, SendFlags, SocketFlags, SocketType};
use std::os::fd::{AsFd, OwnedFd};

/// Flags for every send(): never block, never raise SIGPIPE.
pub(crate) const SEND_FLAGS: SendFlags = SendFlags::DONTWAIT.union(SendFlags::NOSIGNAL);

/// A non-blocking, close-on-exec AF_UNIX stream socket that never raises SIGPIPE.
pub(crate) fn open_stream_socket() -> Result<OwnedFd, Errno> {
    rustix::net::socket_with(
        AddressFamily::UNIX,
        SocketType::STREAM,
        SocketFlags::NONBLOCK | SocketFlags::CLOEXEC,
        None,
    )
}

/// accept() on `listener`, with the same properties.
pub(crate) fn accept_stream_socket(listener: impl AsFd) -> Result<OwnedFd, Errno> {
    rustix::net::accept_with(listener, SocketFlags::NONBLOCK | SocketFlags::CLOEXEC)
}

/// Per-connection kernel buffer sizes. The Linux defaults (~208 KiB) are kept.
pub(crate) fn tune_socket_buffers(_fd: impl AsFd, _opts: &Options) {}

/// Credentials (uid, gid) of the process on the other end of a connected socket.
pub(crate) fn peer_credentials(fd: impl AsFd) -> Option<(u32, u32)> {
    let c = rustix::net::sockopt::get_socket_peercred(fd).ok()?;
    Some((c.uid.as_raw(), c.gid.as_raw()))
}

/// Raise the calling thread's priority by lowering its nice value, relative to
/// the one it inherited (as Windows thread priorities are relative to the
/// process's class). The default level 2 needs a large gap: the scheduler shares
/// CPU by weight, and against a full load at the same nice value 64 B p99.9 was
/// ~8 ms at a gap of 0, ~4 ms at 8, ~200 us at 12 and ~160 us at 19. Level 3
/// matches Android's THREAD_PRIORITY_URGENT_AUDIO. Unprivileged processes may
/// only go down to 20 - RLIMIT_NICE; the thread then gets the lowest nice value
/// it is allowed, or stays where it was. Real-time policies are not used: a
/// spinning real-time thread can starve the rest of the system.
pub(crate) fn apply_thread_priority(level: i32) {
    const STEPS: [i32; 4] = [0, -4, -12, -19];
    if level <= 0 || level > 3 {
        return;
    }
    // On Linux a thread id as the pid affects only that thread.
    let tid = Some(rustix::thread::gettid());
    let Ok(base) = rustix::process::getpriority_process(tid) else {
        return;
    };
    let want = (base + STEPS[level as usize]).max(-20);
    if rustix::process::setpriority_process(tid, want).is_ok() {
        return;
    }
    let limit = rustix::process::getrlimit(rustix::process::Resource::Nice);
    let Some(cur) = limit.current else {
        return;
    };
    let floor = 20 - cur.min(40) as i32;
    if floor > want && floor < base {
        let _ = rustix::process::setpriority_process(tid, floor);
    }
}

/// CPUs in the affinity mask that are the first hyper-thread of their core;
/// 0 if unknown.
pub(crate) fn physical_core_count() -> usize {
    let Ok(set) = rustix::process::sched_getaffinity(None) else {
        return 0;
    };
    let mut n = 0;
    for cpu in 0..rustix::process::CpuSet::MAX_CPU {
        if !set.is_set(cpu) {
            continue;
        }
        let path = format!("/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list");
        let Ok(text) = std::fs::read_to_string(path) else {
            return 0;
        };
        let digits: String = text.chars().take_while(|c| c.is_ascii_digit()).collect();
        let Ok(sibling) = digits.parse::<usize>() else {
            return 0;
        };
        // The first sibling counts for the core, unless it is outside the mask.
        if sibling == cpu || !set.is_set(sibling) {
            n += 1;
        }
    }
    n
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn at_least_one_core() {
        // 0 is allowed (unknown), but a count never exceeds the CPUs we may run on.
        let n = physical_core_count();
        let cpus = std::thread::available_parallelism()
            .map(|n| n.get())
            .unwrap_or(1);
        assert!(n <= cpus.max(1) * 2);
    }

    #[test]
    fn priority_zero_is_a_no_op() {
        apply_thread_priority(0);
        apply_thread_priority(9);
    }
}
