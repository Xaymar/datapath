// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! A context: the I/O threads, and the connections and servers living on them.

use super::conn::{tune, UdsConn};
use super::evloop::{on_io_thread_of, Loop};
use super::listener::Listener;
use super::path::make_socket_address;
use super::platform::{open_stream_socket, physical_core_count, SEND_FLAGS};
use crate::conn::{lock, CloseHandler, MessageHandler};
use crate::error::Error;
use crate::options::{ContextOptions, Options};
use crate::proto::{Hello, HELLO_SIZE};
use rustix::event::{poll, PollFd, PollFlags};
use rustix::io::Errno;
use rustix::net::RecvFlags;
use std::os::fd::OwnedFd;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock, Weak};
use std::time::{Duration, Instant};

#[derive(Default)]
struct Members {
    connections: Vec<Weak<UdsConn>>,
    listeners: Vec<Weak<Listener>>,
}

pub(crate) struct ContextShared {
    opts: ContextOptions,
    loops: OnceLock<Vec<Arc<Loop>>>,
    next: AtomicUsize,
    stopped: AtomicBool,
    members: Mutex<Members>,
}

/// The public handle: dropping the last one shuts the context down.
pub(crate) struct ContextHandle {
    shared: Arc<ContextShared>,
}

impl ContextHandle {
    pub fn new(opts: ContextOptions) -> Result<ContextHandle, Error> {
        Ok(ContextHandle {
            shared: ContextShared::create(opts)?,
        })
    }

    pub fn shared(&self) -> &Arc<ContextShared> {
        &self.shared
    }
}

impl Drop for ContextHandle {
    fn drop(&mut self) {
        self.shared.shutdown();
    }
}

impl ContextShared {
    pub fn create(mut opts: ContextOptions) -> Result<Arc<ContextShared>, Error> {
        if opts.io_threads == 0 {
            // Automatic: one I/O thread per physical core.
            opts.io_threads = physical_core_count();
            if opts.io_threads == 0 {
                opts.io_threads = std::thread::available_parallelism()
                    .map(|n| n.get())
                    .unwrap_or(1);
            }
        }
        opts.io_threads = opts.io_threads.max(1);
        let shared = Arc::new(ContextShared {
            opts,
            loops: OnceLock::new(),
            next: AtomicUsize::new(0),
            stopped: AtomicBool::new(false),
            members: Mutex::new(Members::default()),
        });
        let mut loops = Vec::new();
        for _ in 0..shared.opts.io_threads {
            loops.push(Loop::new().map_err(|_| Error::CriticalFailure)?);
        }
        for l in &loops {
            if l.start(shared.clone()).is_err() {
                // Stop the threads that did start.
                for l in &loops {
                    l.request_quit();
                }
                for l in &loops {
                    l.finish();
                }
                return Err(Error::CriticalFailure);
            }
        }
        let _ = shared.loops.set(loops);
        Ok(shared)
    }

    /// Identifies this context on its I/O threads.
    pub fn id(&self) -> usize {
        self as *const ContextShared as usize
    }

    pub fn options(&self) -> &ContextOptions {
        &self.opts
    }

    pub fn stopped(&self) -> bool {
        self.stopped.load(Ordering::Acquire)
    }

    /// Round-robin loop for a new connection.
    pub fn pick_loop(&self) -> Arc<Loop> {
        let loops = self.loops.get().expect("context is started");
        loops[self.next.fetch_add(1, Ordering::Relaxed) % loops.len()].clone()
    }

    pub fn add_connection(&self, c: Weak<UdsConn>) {
        let mut m = lock(&self.members);
        let n = m.connections.len();
        if n >= 64 && n & (n - 1) == 0 {
            m.connections.retain(|w| w.strong_count() > 0);
        }
        m.connections.push(c);
    }

    pub fn add_listener(&self, l: Weak<Listener>) {
        let mut m = lock(&self.members);
        m.listeners.retain(|w| w.strong_count() > 0);
        m.listeners.push(l);
    }

    pub fn shutdown(self: &Arc<Self>) {
        if self.stopped.swap(true, Ordering::AcqRel) {
            return;
        }
        if on_io_thread_of(self.id()) {
            // An I/O thread cannot join itself, and its caller's stack still uses
            // objects the teardown would free: finish on a separate thread.
            let me = self.clone();
            std::thread::spawn(move || me.shutdown_now());
            return;
        }
        self.shutdown_now();
    }

    fn shutdown_now(&self) {
        let (conns, listeners) = {
            let mut m = lock(&self.members);
            (
                std::mem::take(&mut m.connections),
                std::mem::take(&mut m.listeners),
            )
        };
        for l in listeners.iter().filter_map(Weak::upgrade) {
            l.close();
        }
        for c in conns.iter().filter_map(Weak::upgrade) {
            c.close();
        }
        let Some(loops) = self.loops.get() else {
            return;
        };
        // Close notifications were posted above; the quit item queues behind them.
        for l in loops {
            l.request_quit();
        }
        for l in loops {
            l.finish();
        }
    }

    /// Connect to the server at `path` and complete the handshake.
    pub fn connect(
        self: &Arc<Self>,
        path: &str,
        opts: &Options,
        on_message: Option<MessageHandler>,
        on_close: Option<CloseHandler>,
    ) -> Result<Arc<UdsConn>, Error> {
        if self.stopped() {
            return Err(Error::Closed);
        }
        let addr = make_socket_address(path).ok_or(Error::InvalidPath)?;
        let deadline = Instant::now() + opts.connect_timeout;

        #[cfg_attr(not(target_os = "macos"), allow(unused_mut))]
        let mut fd = open_stream_socket().map_err(|_| Error::CriticalFailure)?;
        tune(&fd, opts);
        loop {
            #[cfg_attr(not(target_os = "macos"), allow(unused_mut))]
            let mut err = match rustix::net::connect_unix(&fd, &addr.addr) {
                Ok(()) => break,
                Err(e) => e,
            };
            if err == Errno::INTR {
                continue;
            }
            // macOS refuses connections while a live server's backlog is full,
            // the same way it does for a socket file without a server. Keep
            // trying while the file exists; the deadline reports TimedOut like
            // a full backlog.
            #[cfg(target_os = "macos")]
            if err == Errno::CONNREFUSED {
                if let Some(path) = &addr.filesystem_path {
                    if matches!(rustix::fs::stat(path.as_str()),
                        Ok(st) if rustix::fs::FileType::from_raw_mode(st.st_mode) == rustix::fs::FileType::Socket)
                    {
                        err = Errno::AGAIN;
                    }
                }
            }
            if err == Errno::AGAIN && Instant::now() < deadline {
                // Backlog full: the server is busy or stalled. Retry until the deadline.
                std::thread::sleep(Duration::from_millis(1));
                // BSD sockets are unusable after a failed connect(): start over.
                #[cfg(target_os = "macos")]
                {
                    fd = open_stream_socket().map_err(|_| Error::CriticalFailure)?;
                    tune(&fd, opts);
                }
                continue;
            }
            return Err(match err {
                Errno::NOENT | Errno::CONNREFUSED => Error::NotFound,
                Errno::ACCESS | Errno::PERM => Error::AccessDenied,
                Errno::AGAIN => Error::TimedOut,
                _ => Error::Failure,
            });
        }

        let mut hello = Hello::new(opts.max_message_size).encode();
        transfer(&fd, true, &mut hello, deadline)?;
        let mut reply = [0u8; HELLO_SIZE];
        transfer(&fd, false, &mut reply, deadline)?;
        let reply = Hello::decode(&reply)?;
        let limit = opts.max_message_size.min(reply.max_message_size);
        let conn = UdsConn::new(self.clone(), self.pick_loop(), fd, opts, limit);
        conn.install_handlers(on_message, on_close);
        if !conn.start(false, Weak::new(), limit) {
            return Err(Error::CriticalFailure);
        }
        self.add_connection(Arc::downgrade(&conn));
        Ok(conn)
    }

    /// Host a server at `path`.
    pub fn listen(self: &Arc<Self>, path: &str, opts: &Options) -> Result<Arc<Listener>, Error> {
        if self.stopped() {
            return Err(Error::Closed);
        }
        let addr = make_socket_address(path).ok_or(Error::InvalidPath)?;
        let listener = Listener::new(self.clone(), self.pick_loop(), addr, opts.clone());
        if let Err(e) = listener.create() {
            listener.close();
            return Err(e);
        }
        self.add_listener(Arc::downgrade(&listener));
        Ok(listener)
    }
}

/// Send or receive all of `buf` before `deadline`.
fn transfer(fd: &OwnedFd, write: bool, buf: &mut [u8], deadline: Instant) -> Result<(), Error> {
    let mut done = 0;
    while done < buf.len() {
        let left = deadline
            .saturating_duration_since(Instant::now())
            .as_millis()
            .min(i32::MAX as u128) as i32;
        let flags = if write { PollFlags::OUT } else { PollFlags::IN };
        let mut fds = [PollFd::new(fd, flags)];
        match poll(&mut fds, left) {
            Err(Errno::INTR) => continue,
            Err(_) => return Err(Error::Closed),
            Ok(0) => return Err(Error::TimedOut),
            Ok(_) => {}
        }
        let r = if write {
            rustix::net::send(fd, &buf[done..], SEND_FLAGS)
        } else {
            rustix::net::recv(fd, &mut buf[done..], RecvFlags::DONTWAIT)
        };
        match r {
            Ok(n) if n > 0 => done += n,
            Err(Errno::INTR) | Err(Errno::AGAIN) => {}
            _ => return Err(Error::Closed),
        }
    }
    Ok(())
}
