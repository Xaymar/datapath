// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! A hosted server: accepts connections and runs the handshake on them.

use super::conn::UdsConn;
use super::context::ContextShared;
use super::evloop::Loop;
use super::path::SocketAddress;
use super::platform::{
    accept_stream_socket, open_stream_socket, peer_credentials, tune_socket_buffers,
};
use super::{ev, Target};
use crate::conn::{lock, ReentrantLock};
use crate::error::Error;
use crate::options::{Options, Permissions};
use rustix::fs::{chmod, chown, lstat, unlink, FileType, Mode};
use rustix::io::Errno;
use std::collections::VecDeque;
use std::os::fd::OwnedFd;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Arc, Mutex, Weak};

const K_OFFER: i32 = 1;

/// Called on an I/O thread for each client that passed the handshake. The
/// callee decides: it either accepts the connection or drops it to reject.
pub(crate) type AcceptHandler = Box<dyn FnMut(Arc<UdsConn>) + Send + 'static>;

struct State {
    fd: Option<OwnedFd>,
    closed: bool,
    unlink_on_close: bool,
    connected: usize,
    handler: Option<Arc<Mutex<AcceptHandler>>>,
    /// Connections that passed the handshake before a handler was set.
    waiting: VecDeque<Arc<UdsConn>>,
    offer_posted: bool,
    conns: Vec<Weak<UdsConn>>,
}

pub(crate) struct Listener {
    me: Weak<Listener>,
    ctx: Arc<ContextShared>,
    loop_: Arc<Loop>,
    addr: SocketAddress,
    opts: Options,
    /// Held while the accept handler runs; `release` takes it to wait it out.
    callback_lock: ReentrantLock,
    state: Mutex<State>,
}

/// True if the socket file at `addr` has no server behind it. Linux refuses
/// connections only when nobody listens (a full backlog is EAGAIN); macOS also
/// refuses them while a live server's backlog is full, so it asks a few times
/// before declaring the file stale.
fn stale_socket(addr: &SocketAddress) -> bool {
    let attempts = if cfg!(target_os = "macos") { 20 } else { 1 };
    for i in 0..attempts {
        if i > 0 {
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        let Ok(probe) = open_stream_socket() else {
            return false;
        };
        if !matches!(
            rustix::net::connect_unix(&probe, &addr.addr),
            Err(Errno::CONNREFUSED)
        ) {
            return false;
        }
    }
    true
}

impl Listener {
    pub fn new(
        ctx: Arc<ContextShared>,
        loop_: Arc<Loop>,
        addr: SocketAddress,
        opts: Options,
    ) -> Arc<Listener> {
        Arc::new_cyclic(|me| Listener {
            me: me.clone(),
            ctx,
            loop_,
            addr,
            opts,
            callback_lock: ReentrantLock::new(),
            state: Mutex::new(State {
                fd: None,
                closed: false,
                unlink_on_close: false,
                connected: 0,
                handler: None,
                waiting: VecDeque::new(),
                offer_posted: false,
                conns: Vec::new(),
            }),
        })
    }

    fn key(&self) -> u64 {
        self as *const Listener as *const () as usize as u64
    }

    pub fn create(&self) -> Result<(), Error> {
        let fd = open_stream_socket().map_err(|_| Error::CriticalFailure)?;
        let do_bind = |fd: &OwnedFd| rustix::net::bind_unix(fd, &self.addr.addr);
        if let Err(e) = do_bind(&fd) {
            if e != Errno::ADDRINUSE {
                return Err(if e == Errno::ACCESS {
                    Error::AccessDenied
                } else {
                    Error::InvalidPath
                });
            }
            let Some(path) = &self.addr.filesystem_path else {
                return Err(Error::AddressInUse);
            };
            // A stale socket file is reused only if it is a socket nobody answers on.
            match lstat(path.as_str()) {
                Ok(st) if FileType::from_raw_mode(st.st_mode) == FileType::Socket => {}
                _ => return Err(Error::AddressInUse),
            }
            if !stale_socket(&self.addr) {
                return Err(Error::AddressInUse);
            }
            let _ = unlink(path.as_str());
            if do_bind(&fd).is_err() {
                return Err(Error::AddressInUse);
            }
        }
        if let Some(path) = &self.addr.filesystem_path {
            lock(&self.state).unlink_on_close = true;
            let mode = match self.opts.permissions {
                Permissions::User => 0o600,
                Permissions::Group => 0o660,
                Permissions::World => 0o666,
            };
            // Before listen(): nobody can connect until the mode is right.
            if chmod(path.as_str(), Mode::from_bits_truncate(mode)).is_err() {
                return Err(Error::AccessDenied);
            }
            // BSDs give a new file the group of its directory, not the creator's:
            // Group access means the creator's group.
            if self.opts.permissions == Permissions::Group
                && chown(path.as_str(), None, Some(rustix::process::getegid())).is_err()
            {
                return Err(Error::AccessDenied);
            }
        }
        rustix::net::listen(&fd, self.opts.backlog.max(16) as i32).map_err(|_| Error::Failure)?;
        let me = self.me.upgrade().ok_or(Error::CriticalFailure)?;
        let mut st = lock(&self.state);
        let fd = st.fd.insert(fd);
        if !self.loop_.add(&*fd, ev::IN, me) {
            return Err(Error::CriticalFailure);
        }
        Ok(())
    }

    fn peer_allowed(&self, fd: &OwnedFd) -> bool {
        if !self.addr.is_abstract() || self.opts.permissions == Permissions::World {
            return true; // filesystem sockets are guarded by their mode bits
        }
        let Some((uid, gid)) = peer_credentials(fd) else {
            return false;
        };
        if uid == 0 || uid == rustix::process::geteuid().as_raw() {
            return true;
        }
        self.opts.permissions == Permissions::Group && gid == rustix::process::getegid().as_raw()
    }

    /// A client passed the handshake: hand it to the accept handler.
    pub fn offer(&self, conn: Arc<UdsConn>) {
        let handler = {
            let mut st = lock(&self.state);
            if st.closed {
                drop(st);
                conn.close();
                return;
            }
            match &st.handler {
                None => {
                    st.waiting.push_back(conn);
                    return;
                }
                Some(h) => h.clone(),
            }
        };
        let _cb = self.callback_lock.lock();
        if lock(&self.state).closed {
            conn.close();
            return;
        }
        // If the handler panics, `conn` is dropped on the way out, which rejects it.
        let _ = catch_unwind(AssertUnwindSafe(|| (*lock(&handler))(conn)));
    }

    pub fn connection_finished(&self) {
        let mut st = lock(&self.state);
        st.connected = st.connected.saturating_sub(1);
    }

    pub fn set_accept_handler(&self, handler: Option<AcceptHandler>) {
        let old = {
            let mut st = lock(&self.state);
            let old = std::mem::replace(&mut st.handler, handler.map(|f| Arc::new(Mutex::new(f))));
            if st.handler.is_some() && !st.waiting.is_empty() && !st.offer_posted {
                if let Some(me) = self.me.upgrade() {
                    st.offer_posted = self.loop_.post(Some(me), K_OFFER);
                }
            }
            old
        };
        // The old handler may own sockets: drop it outside the lock.
        drop(old);
    }

    pub fn close(&self) -> bool {
        let (conns, waiting, handler) = {
            let mut st = lock(&self.state);
            if st.closed {
                return false;
            }
            st.closed = true;
            if let Some(fd) = st.fd.take() {
                self.loop_.remove_fd(&fd);
            }
            if st.unlink_on_close {
                if let Some(path) = &self.addr.filesystem_path {
                    let _ = unlink(path.as_str());
                }
            }
            (
                std::mem::take(&mut st.conns),
                std::mem::take(&mut st.waiting),
                st.handler.take(),
            )
        };
        // The handler may own sockets: drop it outside the lock.
        drop(handler);
        for c in waiting {
            c.close();
        }
        for c in conns.iter().filter_map(Weak::upgrade) {
            c.close();
        }
        // Drop the loop's reference on the loop thread.
        if let Some(me) = self.me.upgrade() {
            self.loop_.post(Some(me), 0);
        }
        true
    }

    /// Close, and wait for a running accept handler.
    pub fn release(&self) {
        self.close();
        drop(self.callback_lock.lock());
    }
}

impl Target for Listener {
    fn on_event(&self, _events: u32) {
        loop {
            let accepted = {
                let st = lock(&self.state);
                match &st.fd {
                    Some(fd) if !st.closed => accept_stream_socket(fd),
                    _ => return,
                }
            };
            let fd = match accepted {
                Ok(fd) => fd,
                Err(Errno::INTR) | Err(Errno::CONNABORTED) => continue,
                // EAGAIN, or out of descriptors: wait for the next edge.
                Err(_) => return,
            };
            let conn = {
                let mut st = lock(&self.state);
                let full = self.opts.max_clients != 0 && st.connected >= self.opts.max_clients;
                if st.closed || full || !self.peer_allowed(&fd) {
                    continue;
                }
                st.connected += 1;
                tune_socket_buffers(&fd, &self.opts);
                let conn = UdsConn::new(
                    self.ctx.clone(),
                    self.ctx.pick_loop(),
                    fd,
                    &self.opts,
                    self.opts.max_message_size,
                );
                st.conns.retain(|w| w.strong_count() > 0);
                st.conns.push(Arc::downgrade(&conn));
                conn
            };
            self.ctx.add_connection(Arc::downgrade(&conn));
            if !conn.start(true, self.me.clone(), self.opts.max_message_size) {
                conn.close();
            }
        }
    }

    fn on_post(&self, kind: i32) {
        if kind == 0 {
            self.loop_.unregister(self.key());
            return;
        }
        if kind != K_OFFER {
            return;
        }
        loop {
            let conn = {
                let mut st = lock(&self.state);
                if st.waiting.is_empty() || st.handler.is_none() {
                    st.offer_posted = false;
                    return;
                }
                st.waiting.pop_front()
            };
            if let Some(c) = conn {
                self.offer(c);
            }
        }
    }
}
