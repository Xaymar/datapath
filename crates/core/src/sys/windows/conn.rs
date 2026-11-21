// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! A stream connection over a named pipe.

use super::context::ContextShared;
use super::listener::Listener;
use super::{reason_for, IoTarget, OpRef, OpSlot};
use crate::conn::{lock, CloseHandler, CloseReason, ConnCore, Hooks, MessageHandler, SendError};
use crate::error::Error;
use crate::options::Options;
use crate::proto::{Hello, HELLO_SIZE};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, RwLock, Weak};
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_INVALID_HANDLE, ERROR_IO_PENDING, HANDLE, INVALID_HANDLE_VALUE,
};
use windows_sys::Win32::Storage::FileSystem::{ReadFile, WriteFile};
use windows_sys::Win32::System::Pipes::DisconnectNamedPipe;
use windows_sys::Win32::System::Threading::Sleep;
use windows_sys::Win32::System::IO::CancelIoEx;

const K_READ: i32 = 1;
const K_WRITE: i32 = 2;
const K_ARM: i32 = 3;
const K_CLOSE: i32 = 4;
const K_HELLO: i32 = 5;

/// One overlapped call never moves more than this.
const MAX_IO: usize = 1 << 30;

enum Io {
    /// Finished at once, with this many bytes.
    Done(u32),
    /// A completion packet will follow.
    Pending,
    /// Failed with this Win32 error.
    Failed(u32),
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

pub(crate) struct PipeConn {
    me: Weak<PipeConn>,
    core: ConnCore,
    ctx: Arc<ContextShared>,
    handle: RwLock<HANDLE>,
    skip_on_success: bool,
    rop: OpSlot,
    wop: OpSlot,
    aop: OpSlot,
    cop: OpSlot,
    hop: OpSlot,
    wstate: Mutex<WriteState>,
    hs: Mutex<Handshake>,
    server_side: AtomicBool,
    /// A graceful close lets the client read what was written before the
    /// server end is disconnected.
    graceful: AtomicBool,
}

impl PipeConn {
    pub fn new(
        ctx: Arc<ContextShared>,
        h: HANDLE,
        skip_on_success: bool,
        opts: &Options,
        negotiated_limit: u64,
    ) -> Arc<PipeConn> {
        Arc::new_cyclic(|me| PipeConn {
            me: me.clone(),
            core: ConnCore::new(negotiated_limit, opts.receive_buffer, opts.max_queued_bytes),
            ctx,
            handle: RwLock::new(h),
            skip_on_success,
            rop: OpSlot::new(K_READ),
            wop: OpSlot::new(K_WRITE),
            aop: OpSlot::new(K_ARM),
            cop: OpSlot::new(K_CLOSE),
            hop: OpSlot::new(K_HELLO),
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
            server_side: AtomicBool::new(false),
            graceful: AtomicBool::new(false),
        })
    }

    fn me(&self) -> Option<Arc<PipeConn>> {
        self.me.upgrade()
    }

    /// Store handlers before `start_client`.
    pub fn install_handlers(
        &self,
        on_message: Option<MessageHandler>,
        on_close: Option<CloseHandler>,
    ) {
        self.core.install_handlers(on_message, on_close);
    }

    /// Client side: begin reading if handlers were installed up front.
    pub fn start_client(&self) {
        if self.core.armed() {
            self.hook_arm();
        }
    }

    /// Server side: hold the writer slot, read and validate the client's
    /// hello, then offer the connection to the listener.
    pub fn begin_server_handshake(&self, listener: Weak<Listener>, own_limit: u64) {
        self.server_side.store(true, Ordering::Release);
        {
            let mut hs = lock(&self.hs);
            hs.active = true;
            hs.listener = listener;
            hs.own_limit = own_limit;
        }
        // Replies must go out before anything the user queues.
        self.core.queue.try_acquire();
        self.hello_loop();
    }

    /// Start one overlapped read or write.
    fn issue(&self, write: bool, buf: *mut u8, len: usize, slot: &OpSlot) -> Io {
        let guard = self.handle.read().unwrap_or_else(|e| e.into_inner());
        let h = *guard;
        if h == INVALID_HANDLE_VALUE {
            return Io::Failed(ERROR_INVALID_HANDLE);
        }
        let Some(me) = self.me() else {
            return Io::Failed(ERROR_INVALID_HANDLE);
        };
        let n = len.min(MAX_IO) as u32;
        // SAFETY: no operation is pending on `slot` (each slot has one owner at
        // a time, see `OpSlot`). `buf` points into memory owned by `self`
        // (parser buffer, write batch or hello bytes), which stays put until
        // the completion: `slot` keeps `self` alive until then.
        unsafe {
            let ov = slot.arm(me);
            self.ctx.op_started();
            let mut bytes = 0u32;
            let ok = if write {
                WriteFile(h, buf, n, &mut bytes, ov)
            } else {
                ReadFile(h, buf, n, &mut bytes, ov)
            };
            if ok != 0 {
                if !self.skip_on_success {
                    return Io::Pending; // a completion packet follows anyway
                }
                slot.disarm();
                self.ctx.op_finished();
                return Io::Done(bytes);
            }
            let err = GetLastError();
            if err == ERROR_IO_PENDING {
                return Io::Pending;
            }
            slot.disarm();
            self.ctx.op_finished();
            Io::Failed(err)
        }
    }

    fn fail(&self, err: u32) {
        self.core.close_with(reason_for(err), self);
    }

    fn read_loop(&self) {
        loop {
            if self.core.is_closed() {
                self.core.finish_close(self);
                return;
            }
            let (ptr, len) = {
                let mut parser = lock(&self.core.parser);
                let area = parser.prepare();
                (area.as_mut_ptr(), area.len())
            };
            match self.issue(false, ptr, len, &self.rop) {
                Io::Done(n) => {
                    let mut parser = lock(&self.core.parser);
                    if !self.core.consume(&mut parser, n as usize, self) {
                        return;
                    }
                }
                Io::Pending => return,
                Io::Failed(err) => {
                    self.fail(err);
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
            let off = ws.off;
            let len = ws.batch.len() - off;
            // SAFETY: the pointer is into the batch's heap block, which is not
            // touched until this write completes (the writer slot is ours).
            let ptr = unsafe { ws.batch.as_mut_ptr().add(off) };
            match self.issue(true, ptr, len, &self.wop) {
                Io::Done(n) => ws.off += n as usize,
                Io::Pending => return,
                Io::Failed(err) => {
                    self.fail(err);
                    return;
                }
            }
        }
    }

    fn hello_loop(&self) {
        loop {
            let mut hs = lock(&self.hs);
            if !hs.active || self.core.is_closed() {
                return;
            }
            let got = hs.got;
            // SAFETY: the pointer is into `self`, untouched until completion.
            let ptr = unsafe { hs.hello.as_mut_ptr().add(got) };
            match self.issue(false, ptr, HELLO_SIZE - got, &self.hop) {
                Io::Done(n) => {
                    hs.got += n as usize;
                    if hs.got == HELLO_SIZE {
                        self.hello_done(hs);
                        return;
                    }
                }
                Io::Pending => return,
                Io::Failed(err) => {
                    drop(hs);
                    self.fail(err);
                    return;
                }
            }
        }
    }

    fn hello_done(&self, mut hs: std::sync::MutexGuard<'_, Handshake>) {
        hs.active = false;
        let hello = Hello::decode(&hs.hello);
        let own_limit = hs.own_limit;
        let listener = hs.listener.upgrade();
        drop(hs);
        let Ok(h) = hello else {
            // Answer with our own hello so the client can report
            // VersionMismatch, then close once it is written.
            self.graceful.store(true, Ordering::Release);
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

    pub fn close(&self) -> bool {
        self.core.close_with(CloseReason::Local, self)
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

    fn hook_arm(&self) {
        // Hop onto an I/O thread so messages are never delivered on the caller's thread.
        let posted = match self.me() {
            Some(me) => self.ctx.post(&self.aop, me),
            None => false,
        };
        if !posted {
            self.core.close_with(CloseReason::Local, self);
            self.core.finish_close(self);
        }
    }
}

impl Drop for PipeConn {
    fn drop(&mut self) {
        let h = *self.handle.get_mut().unwrap_or_else(|e| e.into_inner());
        if h != INVALID_HANDLE_VALUE {
            // SAFETY: the handle is ours; nothing is pending (we are unreachable).
            unsafe { CloseHandle(h) };
        }
    }
}

impl Hooks for PipeConn {
    fn platform_close(&self) {
        let mut guard = self.handle.write().unwrap_or_else(|e| e.into_inner());
        let h = *guard;
        if h != INVALID_HANDLE_VALUE {
            // SAFETY: `h` is open; the write lock keeps `issue` out meanwhile.
            unsafe {
                // Disconnect the server end so the pipe name is released even
                // while the client still holds its handle (otherwise re-hosting
                // fails). Skipped for a graceful close that must let the client
                // read what was written.
                if self.server_side.load(Ordering::Acquire)
                    && !self.graceful.load(Ordering::Acquire)
                {
                    DisconnectNamedPipe(h);
                }
                CancelIoEx(h, std::ptr::null());
                CloseHandle(h);
            }
            *guard = INVALID_HANDLE_VALUE;
        }
    }

    fn platform_arm(&self) {
        self.hook_arm();
    }

    fn platform_post_close(&self) {
        let posted = match self.me() {
            Some(me) => self.ctx.post(&self.cop, me),
            None => false,
        };
        if !posted {
            self.core.finish_close(self);
        }
    }

    fn platform_write(&self) {
        self.write_loop();
    }

    fn after_inline_write(&self) {
        // Let a receiver that our write just woke run on this core right away.
        if self.ctx.options().yield_cpu {
            // SAFETY: plain Win32 call.
            unsafe { Sleep(0) };
        }
    }

    fn on_finished(&self) {
        if let Some(l) = lock(&self.hs).listener.upgrade() {
            l.connection_finished();
        }
    }
}

impl IoTarget for PipeConn {
    fn on_io(&self, op: OpRef, bytes: u32, err: u32) {
        match op.kind() {
            K_READ => {
                if err != 0 {
                    self.fail(err);
                    self.core.finish_close(self);
                    return;
                }
                let more = {
                    let mut parser = lock(&self.core.parser);
                    self.core.consume(&mut parser, bytes as usize, self)
                };
                if more {
                    self.read_loop();
                }
            }
            K_WRITE => {
                if err != 0 {
                    // Keep the writer slot: the queue is closed, nothing more goes out.
                    self.fail(err);
                    return;
                }
                lock(&self.wstate).off += bytes as usize;
                self.write_loop();
            }
            K_ARM => self.read_loop(),
            K_CLOSE => self.core.finish_close(self),
            K_HELLO => {
                if err != 0 {
                    self.fail(err);
                    return;
                }
                let mut hs = lock(&self.hs);
                hs.got += bytes as usize;
                if hs.got == HELLO_SIZE {
                    self.hello_done(hs);
                } else {
                    drop(hs);
                    self.hello_loop();
                }
            }
            _ => {}
        }
    }
}
