// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! A stream connection over an AF_UNIX socket.

use super::context::ContextShared;
use super::evloop::Loop;
use super::listener::Listener;
use super::platform::{tune_socket_buffers, SEND_FLAGS};
use super::{ev, Target};
use crate::conn::{lock, CloseHandler, CloseReason, ConnCore, Hooks, MessageHandler, SendError};
use crate::error::Error;
use crate::options::Options;
use crate::proto::{Hello, HELLO_SIZE};
use rustix::io::Errno;
use rustix::net::{RecvFlags, Shutdown};
use std::os::fd::OwnedFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, RwLock, Weak};

const K_ARM: i32 = 1;
const K_CLOSED: i32 = 2;

/// Orderly or abrupt end of the peer.
fn gone(e: Errno) -> bool {
    matches!(
        e,
        Errno::PIPE | Errno::CONNRESET | Errno::NOTCONN | Errno::BADF | Errno::SHUTDOWN
    )
}

fn reason_for(e: Errno) -> CloseReason {
    if gone(e) {
        CloseReason::Remote
    } else {
        CloseReason::Io(Error::Failure)
    }
}

struct WriteState {
    batch: Vec<u8>,
    off: usize,
    /// Close once the reply hello has been written (version mismatch).
    close_when_flushed: bool,
}

struct Handshake {
    active: bool,
    hello: [u8; HELLO_SIZE],
    got: usize,
    own_limit: u64,
    listener: Weak<Listener>,
}

pub(crate) struct UdsConn {
    me: Weak<UdsConn>,
    core: ConnCore,
    ctx: Arc<ContextShared>,
    loop_: Arc<Loop>,
    fd: RwLock<Option<OwnedFd>>,
    mask: Mutex<u32>,
    /// The writer slot is parked with the loop: it resumes on `OUT`.
    write_parked: AtomicBool,
    wstate: Mutex<WriteState>,
    hs: Mutex<Handshake>,
}

impl UdsConn {
    pub fn new(
        ctx: Arc<ContextShared>,
        loop_: Arc<Loop>,
        fd: OwnedFd,
        opts: &Options,
        negotiated_limit: u64,
    ) -> Arc<UdsConn> {
        Arc::new_cyclic(|me| UdsConn {
            me: me.clone(),
            core: ConnCore::new(negotiated_limit, opts.receive_buffer, opts.max_queued_bytes),
            ctx,
            loop_,
            fd: RwLock::new(Some(fd)),
            mask: Mutex::new(ev::RDHUP),
            write_parked: AtomicBool::new(false),
            wstate: Mutex::new(WriteState {
                batch: Vec::new(),
                off: 0,
                close_when_flushed: false,
            }),
            hs: Mutex::new(Handshake {
                active: false,
                hello: [0; HELLO_SIZE],
                got: 0,
                own_limit: 0,
                listener: Weak::new(),
            }),
        })
    }

    fn me(&self) -> Option<Arc<UdsConn>> {
        self.me.upgrade()
    }

    /// Register with the loop. A server-side connection (`handshake`) first
    /// reads the client's hello.
    pub fn start(
        self: &Arc<Self>,
        handshake: bool,
        listener: Weak<Listener>,
        own_limit: u64,
    ) -> bool {
        let guard = self.fd.read().unwrap_or_else(|e| e.into_inner());
        let mut mask = lock(&self.mask);
        if handshake {
            let mut hs = lock(&self.hs);
            hs.active = true;
            hs.listener = listener;
            hs.own_limit = own_limit;
            // The reply goes out before anything the user queues.
            self.core.queue.try_acquire();
            *mask |= ev::IN;
        }
        if self.core.armed() {
            *mask |= ev::IN;
        }
        match &*guard {
            Some(fd) => self.loop_.add(fd, *mask, self.clone()),
            None => false,
        }
    }

    /// Store handlers before `start`.
    pub fn install_handlers(
        &self,
        on_message: Option<MessageHandler>,
        on_close: Option<CloseHandler>,
    ) {
        self.core.install_handlers(on_message, on_close);
    }

    fn update_mask(&self, add: u32, remove: u32) {
        let guard = self.fd.read().unwrap_or_else(|e| e.into_inner());
        let Some(fd) = &*guard else { return };
        let mut mask = lock(&self.mask);
        let new = (*mask | add) & !remove;
        if new == *mask {
            return;
        }
        self.loop_.modify(fd, new, key_of_self(self));
        *mask = new;
    }

    fn read_loop(&self) {
        loop {
            if self.core.is_closed() {
                self.core.finish_close(self);
                return;
            }
            let mut parser = lock(&self.core.parser);
            let result = {
                let guard = self.fd.read().unwrap_or_else(|e| e.into_inner());
                match &*guard {
                    None => Err(Errno::BADF),
                    Some(fd) => rustix::net::recv(fd, parser.prepare(), RecvFlags::DONTWAIT),
                }
            };
            match result {
                Ok(n) if n > 0 => {
                    if !self.core.consume(&mut parser, n, self) {
                        return;
                    }
                }
                Ok(_) => {
                    drop(parser);
                    self.core.close_with(CloseReason::Remote, self);
                    self.core.finish_close(self);
                    return;
                }
                Err(Errno::INTR) => {}
                Err(Errno::AGAIN) => return,
                Err(e) => {
                    drop(parser);
                    self.core.close_with(reason_for(e), self);
                    self.core.finish_close(self);
                    return;
                }
            }
        }
    }

    fn write_loop(&self) {
        let mut ws = lock(&self.wstate);
        loop {
            if ws.off >= ws.batch.len() {
                ws.off = 0;
                if !self.core.queue.take(&mut ws.batch) {
                    if ws.close_when_flushed {
                        self.core
                            .close_with(CloseReason::Protocol(Error::VersionMismatch), self);
                    }
                    return;
                }
            }
            let result = {
                let guard = self.fd.read().unwrap_or_else(|e| e.into_inner());
                match &*guard {
                    None => return,
                    Some(fd) => rustix::net::send(fd, &ws.batch[ws.off..], SEND_FLAGS),
                }
            };
            match result {
                Ok(n) if n > 0 => ws.off += n,
                Ok(_) => {}
                Err(Errno::INTR) => {}
                Err(Errno::AGAIN) => {
                    // Hand the writer slot to the loop: it resumes on OUT.
                    // Enabling OUT re-checks readiness, so no wake-up is lost.
                    self.write_parked.store(true, Ordering::Release);
                    self.update_mask(ev::OUT, 0);
                    return;
                }
                Err(e) => {
                    self.core.close_with(reason_for(e), self);
                    return;
                }
            }
        }
    }

    fn hello_read(&self) {
        loop {
            let mut hs = lock(&self.hs);
            if !hs.active || self.core.is_closed() {
                return;
            }
            let result = {
                let guard = self.fd.read().unwrap_or_else(|e| e.into_inner());
                let Some(fd) = &*guard else { return };
                let got = hs.got;
                rustix::net::recv(fd, &mut hs.hello[got..], RecvFlags::DONTWAIT)
            };
            match result {
                Ok(n) if n > 0 => {
                    hs.got += n;
                    if hs.got < HELLO_SIZE {
                        continue;
                    }
                    hs.active = false;
                    // Data waits in the kernel until reads are armed.
                    self.update_mask(0, ev::IN);
                    let hello = Hello::decode(&hs.hello);
                    let own_limit = hs.own_limit;
                    let listener = hs.listener.upgrade();
                    drop(hs);
                    let Ok(h) = hello else {
                        // Answer with our own hello so the client can report
                        // VersionMismatch, then close once it is written.
                        lock(&self.wstate).close_when_flushed = true;
                        self.complete_accept();
                        return;
                    };
                    let limit = own_limit.min(h.max_message_size);
                    lock(&self.core.parser).set_limit(limit);
                    self.core.queue.set_limit(limit);
                    match (listener, self.me()) {
                        (Some(l), Some(me)) => l.offer(me),
                        _ => {
                            self.core.close_with(CloseReason::Local, self);
                        }
                    }
                    return;
                }
                Ok(_) => {
                    drop(hs);
                    self.core.close_with(CloseReason::Remote, self);
                    return;
                }
                Err(Errno::INTR) => {}
                Err(Errno::AGAIN) => return,
                Err(e) => {
                    drop(hs);
                    self.core.close_with(reason_for(e), self);
                    return;
                }
            }
        }
    }

    /// The server accepted this connection: send the reply hello, then whatever
    /// the user queued.
    pub fn complete_accept(&self) {
        {
            let mut ws = lock(&self.wstate);
            let own_limit = lock(&self.hs).own_limit;
            ws.batch = Hello::new(own_limit).encode().to_vec();
            ws.off = 0;
        }
        self.write_loop();
    }

    // The operations the public API forwards to --------------------------------

    pub fn send(&self, parts: &[&[u8]]) -> Result<(), SendError> {
        self.core.send(parts, self)
    }

    pub fn set_message_handler(&self, handler: Option<MessageHandler>) {
        self.core.set_message_handler(handler, self);
    }

    pub fn set_close_handler(&self, handler: Option<CloseHandler>) {
        self.core.set_close_handler(handler);
    }

    pub fn close(&self) {
        self.core.close_with(CloseReason::Local, self);
    }

    pub fn is_open(&self) -> bool {
        !self.core.is_closed()
    }

    pub fn max_message_size(&self) -> u64 {
        self.core.queue.limit()
    }

    pub fn release(&self) {
        self.core.release(self);
    }
}

fn key_of_self(c: &UdsConn) -> u64 {
    // The registration key is the address of the Arc's contents.
    c as *const UdsConn as *const () as usize as u64
}

impl Hooks for UdsConn {
    fn platform_close(&self) {
        {
            let mut guard = self.fd.write().unwrap_or_else(|e| e.into_inner());
            if let Some(fd) = guard.take() {
                self.loop_.remove_fd(&fd);
                let _ = rustix::net::shutdown(&fd, Shutdown::ReadWrite);
                drop(fd);
            }
        }
        // The fd no longer produces events: always notify the loop explicitly.
        let posted = match self.me() {
            Some(me) => self.loop_.post(Some(me), K_CLOSED),
            None => false,
        };
        if !posted {
            self.core.finish_close(self);
        }
    }

    fn platform_arm(&self) {
        let posted = match self.me() {
            Some(me) => self.loop_.post(Some(me), K_ARM),
            None => false,
        };
        if !posted {
            self.core.close_with(CloseReason::Local, self);
            self.core.finish_close(self);
        }
    }

    fn platform_post_close(&self) {
        // platform_close() already posted K_CLOSED.
    }

    fn platform_write(&self) {
        self.write_loop();
    }

    fn after_inline_write(&self) {
        if self.ctx.options().yield_cpu {
            std::thread::yield_now();
        }
    }

    fn on_finished(&self) {
        if let Some(l) = lock(&self.hs).listener.upgrade() {
            l.connection_finished();
        }
    }
}

impl Target for UdsConn {
    fn on_event(&self, events: u32) {
        if events & (ev::OUT | ev::ERR | ev::HUP) != 0
            && self.write_parked.swap(false, Ordering::AcqRel)
        {
            self.update_mask(0, ev::OUT);
            self.write_loop();
        }
        if events & (ev::IN | ev::RDHUP | ev::HUP | ev::ERR) != 0 {
            let handshaking = lock(&self.hs).active;
            if handshaking {
                self.hello_read();
            } else if self.core.armed() {
                self.read_loop();
            }
        }
    }

    fn on_post(&self, kind: i32) {
        match kind {
            K_ARM => {
                self.update_mask(ev::IN, 0);
                self.read_loop();
            }
            K_CLOSED => {
                self.core.finish_close(self);
                self.loop_.unregister(key_of_self(self));
            }
            _ => {}
        }
    }
}

/// Used by the context to tune a freshly created client socket.
pub(crate) fn tune(fd: &OwnedFd, opts: &Options) {
    tune_socket_buffers(fd, opts);
}
