// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! macOS.

// Two thread-level calls (quality-of-service class, physical core count) have no
// safe wrapper in `rustix`; each is a single FFI call with plain arguments.
#![allow(unsafe_code)]

use crate::options::Options;
use rustix::fs::{fcntl_getfl, fcntl_setfd, fcntl_setfl, FdFlags, OFlags};
use rustix::io::Errno;
use rustix::net::{AddressFamily, SendFlags, SocketType};
use std::ffi::{c_char, c_int, c_uint, c_void};
use std::os::fd::{AsFd, OwnedFd};

/// Flags for every send(). macOS has no `MSG_NOSIGNAL`: sockets get
/// `SO_NOSIGPIPE` when they are created instead.
pub(crate) const SEND_FLAGS: SendFlags = SendFlags::DONTWAIT;

/// Close-on-exec, non-blocking and no SIGPIPE.
fn prepare(fd: OwnedFd) -> Result<OwnedFd, Errno> {
    fcntl_setfd(&fd, FdFlags::CLOEXEC)?;
    fcntl_setfl(&fd, fcntl_getfl(&fd)? | OFlags::NONBLOCK)?;
    rustix::net::sockopt::set_socket_nosigpipe(&fd, true)?;
    Ok(fd)
}

/// A non-blocking, close-on-exec AF_UNIX stream socket that never raises SIGPIPE.
pub(crate) fn open_stream_socket() -> Result<OwnedFd, Errno> {
    prepare(rustix::net::socket(
        AddressFamily::UNIX,
        SocketType::STREAM,
        None,
    )?)
}

/// accept() on `listener`, with the same properties.
pub(crate) fn accept_stream_socket(listener: impl AsFd) -> Result<OwnedFd, Errno> {
    let fd = rustix::net::accept(listener)?;
    // setsockopt() fails with EINVAL once both directions are shut down, as on a
    // connection whose client already closed. Report it like a connection
    // aborted before accept(), so the caller drops it and keeps accepting
    // (otherwise the clients queued behind it wait for the next edge).
    prepare(fd).map_err(|_| Errno::CONNABORTED)
}

/// Unix stream sockets start with 8 KiB buffers (`net.local.stream.sendspace`
/// and `recvspace`), so a 64 KiB message can take several wake-ups of the
/// reader. Raise both to `pipe_buffer` (or `receive_buffer` when that is 0),
/// halving until the kernel accepts it: `kern.ipc.maxsockbuf` caps the size.
pub(crate) fn tune_socket_buffers(fd: impl AsFd, opts: &Options) {
    let want = if opts.pipe_buffer != 0 {
        opts.pipe_buffer
    } else {
        opts.receive_buffer
    }
    .min(1 << 30);
    let fd = fd.as_fd();
    let mut size = want;
    while size >= 8192 {
        if rustix::net::sockopt::set_socket_send_buffer_size(fd, size).is_ok() {
            break;
        }
        size /= 2;
    }
    let mut size = want;
    while size >= 8192 {
        if rustix::net::sockopt::set_socket_recv_buffer_size(fd, size).is_ok() {
            break;
        }
        size /= 2;
    }
}

/// Credentials of the peer. Only abstract sockets (which macOS does not have)
/// are checked against them, so nothing asks.
pub(crate) fn peer_credentials(_fd: impl AsFd) -> Option<(u32, u32)> {
    None
}

extern "C" {
    fn pthread_set_qos_class_self_np(qos_class: c_uint, relative_priority: c_int) -> c_int;
    fn sysctlbyname(
        name: *const c_char,
        oldp: *mut c_void,
        oldlenp: *mut usize,
        newp: *mut c_void,
        newlen: usize,
    ) -> c_int;
}

const QOS_CLASS_USER_INTERACTIVE: c_uint = 0x21;
const QOS_CLASS_USER_INITIATED: c_uint = 0x19;

/// The scheduler picks threads, cores (performance or efficiency) and timer
/// coalescing by quality-of-service class rather than by priority: 1 =
/// user-initiated, 2 and 3 = user-interactive, the class of a UI's main thread.
/// Unlike nice values, these need no privileges. Real-time (time constraint)
/// scheduling is not used: a spinning real-time thread is demoted by the kernel
/// once it overruns its computation budget, which would show up as a tail stall.
pub(crate) fn apply_thread_priority(level: i32) {
    let class = match level {
        1 => QOS_CLASS_USER_INITIATED,
        2 | 3 => QOS_CLASS_USER_INTERACTIVE,
        _ => return,
    };
    // SAFETY: plain integer arguments; affects only the calling thread.
    unsafe {
        pthread_set_qos_class_self_np(class, 0);
    }
}

/// Physical cores (`hw.physicalcpu`); 0 if unknown.
pub(crate) fn physical_core_count() -> usize {
    let mut n: c_int = 0;
    let mut len = std::mem::size_of::<c_int>();
    // SAFETY: `n` and `len` are valid for the sizes passed; the name is NUL-terminated.
    let rc = unsafe {
        sysctlbyname(
            b"hw.physicalcpu\0".as_ptr().cast(),
            (&mut n as *mut c_int).cast(),
            &mut len,
            std::ptr::null_mut(),
            0,
        )
    };
    if rc != 0 || n <= 0 {
        0
    } else {
        n as usize
    }
}
