// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! A hosted server: keeps pipe instances ready, accepts connections and runs
//! the handshake on them.

use super::conn::PipeConn;
use super::context::ContextShared;
use super::security::SecurityDescriptor;
use super::{map_error, IoTarget, OpRef, OpSlot};
use crate::conn::{lock, ReentrantLock};
use crate::error::Error;
use crate::options::Options;
use std::collections::VecDeque;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Arc, Mutex, Weak};
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_ACCESS_DENIED, ERROR_FILENAME_EXCED_RANGE, ERROR_INVALID_NAME,
    ERROR_IO_PENDING, ERROR_NO_DATA, ERROR_PIPE_BUSY, ERROR_PIPE_CONNECTED, ERROR_SEM_TIMEOUT,
    HANDLE, INVALID_HANDLE_VALUE,
};
use windows_sys::Win32::Security::SECURITY_ATTRIBUTES;
use windows_sys::Win32::Storage::FileSystem::{
    FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OVERLAPPED, PIPE_ACCESS_DUPLEX,
};
use windows_sys::Win32::System::Pipes::{
    ConnectNamedPipe, CreateNamedPipeW, WaitNamedPipeW, PIPE_READMODE_BYTE,
    PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES, PIPE_WAIT,
};
use windows_sys::Win32::System::IO::CancelIoEx;

const K_CONNECT: i32 = 1;
const K_OFFER: i32 = 2;

/// Called on an I/O thread for each client that passed the handshake. The
/// callee decides: it either accepts the connection or drops it to reject.
pub(crate) type AcceptHandler = Box<dyn FnMut(Arc<PipeConn>) + Send + 'static>;

/// A pipe instance waiting for a client.
struct Instance {
    h: HANDLE,
    skip: bool,
    op: OpSlot,
}

impl Drop for Instance {
    fn drop(&mut self) {
        if self.h != INVALID_HANDLE_VALUE {
            // SAFETY: the handle is ours; a pending connect (if any) completes
            // with an abort and only touches `self.op` through its own keep.
            unsafe { CloseHandle(self.h) };
        }
    }
}

struct State {
    closed: bool,
    connected: usize,
    sd: Option<SecurityDescriptor>,
    // Boxed: a pending connect holds the address of the instance's OVERLAPPED.
    #[allow(clippy::vec_box)]
    pending: Vec<Box<Instance>>,
    handler: Option<Arc<Mutex<AcceptHandler>>>,
    /// Connections that passed the handshake before a handler was set.
    waiting: VecDeque<Arc<PipeConn>>,
    offer_posted: bool,
    conns: Vec<Weak<PipeConn>>,
}

pub(crate) struct Listener {
    me: Weak<Listener>,
    ctx: Arc<ContextShared>,
    path: Vec<u16>,
    opts: Options,
    offer_op: OpSlot,
    /// Held while the accept handler runs; `release` takes it to wait it out.
    callback_lock: ReentrantLock,
    state: Mutex<State>,
}

impl Listener {
    pub fn new(ctx: Arc<ContextShared>, path: Vec<u16>, mut opts: Options) -> Arc<Listener> {
        if opts.backlog == 0 {
            opts.backlog = 1;
        }
        Arc::new_cyclic(|me| Listener {
            me: me.clone(),
            ctx,
            path,
            opts,
            offer_op: OpSlot::new(K_OFFER),
            callback_lock: ReentrantLock::new(),
            state: Mutex::new(State {
                closed: false,
                connected: 0,
                sd: None,
                pending: Vec::new(),
                handler: None,
                waiting: VecDeque::new(),
                offer_posted: false,
                conns: Vec::new(),
            }),
        })
    }

    fn me(&self) -> Option<Arc<Listener>> {
        self.me.upgrade()
    }

    pub fn create(&self) -> Result<(), Error> {
        let sd = SecurityDescriptor::new(self.opts.permissions).ok_or(Error::CriticalFailure)?;
        // An existing server makes WaitNamedPipe succeed or time out instead of
        // reporting ERROR_FILE_NOT_FOUND. FILE_FLAG_FIRST_PIPE_INSTANCE below
        // is the authoritative check; this one also covers emulators that
        // ignore the flag.
        // SAFETY: the path is NUL-terminated.
        let busy = unsafe {
            WaitNamedPipeW(self.path.as_ptr(), 1) != 0 || GetLastError() == ERROR_SEM_TIMEOUT
        };
        if busy {
            return Err(Error::AddressInUse);
        }
        let mut st = lock(&self.state);
        st.sd = Some(sd);
        self.spawn_locked(&mut st, true)?;
        self.refill_locked(&mut st);
        Ok(())
    }

    /// Create one pipe instance and start waiting for a client on it.
    fn spawn_locked(&self, st: &mut State, mut first: bool) -> Result<(), Error> {
        let mut open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if first {
            open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        }
        let pipe_mode =
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
        let wanted = if self.opts.pipe_buffer != 0 {
            self.opts.pipe_buffer
        } else {
            self.opts.receive_buffer
        };
        let buffer = wanted.min(64 << 20) as u32;
        let sa = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: st.sd.as_ref().map_or(std::ptr::null_mut(), |s| s.as_ptr()),
            bInheritHandle: 0,
        };
        let me = self.me().ok_or(Error::CriticalFailure)?;

        for _ in 0..4 {
            // SAFETY: the path is NUL-terminated; `sa` outlives the call.
            let h = unsafe {
                CreateNamedPipeW(
                    self.path.as_ptr(),
                    open_mode,
                    pipe_mode,
                    PIPE_UNLIMITED_INSTANCES,
                    buffer,
                    buffer,
                    0,
                    &sa,
                )
            };
            if h == INVALID_HANDLE_VALUE {
                // SAFETY: reads the calling thread's last error.
                let err = unsafe { GetLastError() };
                if first && (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY) {
                    return Err(Error::AddressInUse);
                }
                if err == ERROR_INVALID_NAME || err == ERROR_FILENAME_EXCED_RANGE {
                    return Err(Error::InvalidPath);
                }
                return Err(map_error(err));
            }
            let skip = match self.ctx.associate(h) {
                Ok(s) => s,
                Err(e) => {
                    // SAFETY: `h` is ours and unused.
                    unsafe { CloseHandle(h) };
                    return Err(e);
                }
            };
            let inst = Box::new(Instance {
                h,
                skip,
                op: OpSlot::new(K_CONNECT),
            });
            // SAFETY: nothing is pending on the new slot; the box keeps the
            // OVERLAPPED at a fixed address until the completion is dispatched
            // (the keep reference holds the listener, which owns the box).
            let (ok, err) = unsafe {
                let ov = inst.op.arm(me.clone());
                self.ctx.op_started();
                let ok = ConnectNamedPipe(h, ov);
                (
                    ok != 0,
                    if ok != 0 {
                        ERROR_PIPE_CONNECTED
                    } else {
                        GetLastError()
                    },
                )
            };
            if err == ERROR_IO_PENDING && !ok {
                st.pending.push(inst);
                return Ok(());
            }
            // SAFETY: no completion packet follows a synchronous result.
            unsafe { inst.op.disarm() };
            self.ctx.op_finished();
            if err == ERROR_PIPE_CONNECTED {
                // A client was already waiting: complete it on an I/O thread.
                st.pending.push(inst);
                let raw = st.pending.last().expect("just pushed");
                if !self.ctx.post(&raw.op, me.clone()) {
                    st.pending.pop();
                    return Err(Error::Closed);
                }
                return Ok(());
            }
            // ERROR_NO_DATA: a client connected and left already; try a fresh instance.
            drop(inst);
            if err != ERROR_NO_DATA {
                return Err(map_error(err));
            }
            first = false;
            open_mode &= !FILE_FLAG_FIRST_PIPE_INSTANCE;
        }
        Err(Error::Failure)
    }

    fn refill_locked(&self, st: &mut State) {
        while !st.closed
            && st.pending.len() < self.opts.backlog
            && (self.opts.max_clients == 0
                || st.connected + st.pending.len() < self.opts.max_clients)
        {
            if self.spawn_locked(st, false).is_err() {
                break;
            }
        }
    }

    /// A client passed the handshake: hand it to the accept handler.
    pub fn offer(&self, conn: Arc<PipeConn>) {
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
        // `release` may have run while we waited for the callback lock.
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
        self.refill_locked(&mut st);
    }

    pub fn set_accept_handler(&self, handler: Option<AcceptHandler>) {
        let old = {
            let mut st = lock(&self.state);
            let old = std::mem::replace(&mut st.handler, handler.map(|f| Arc::new(Mutex::new(f))));
            if st.handler.is_some() && !st.waiting.is_empty() && !st.offer_posted {
                if let Some(me) = self.me() {
                    st.offer_posted = self.ctx.post(&self.offer_op, me);
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
            for inst in st.pending.iter_mut() {
                if inst.h != INVALID_HANDLE_VALUE {
                    // SAFETY: the handle is ours; closing makes the pending
                    // connect complete with an abort.
                    unsafe {
                        CancelIoEx(inst.h, std::ptr::null());
                        CloseHandle(inst.h);
                    }
                    inst.h = INVALID_HANDLE_VALUE;
                }
            }
            (
                std::mem::take(&mut st.conns),
                std::mem::take(&mut st.waiting),
                st.handler.take(),
            )
        };
        // The pending instances stay: their aborted connects complete on the
        // I/O threads, and the kernel may still reference their OVERLAPPED.
        // The handler may own sockets: drop it outside the lock.
        drop(handler);
        for c in waiting {
            c.close();
        }
        for c in conns.iter().filter_map(Weak::upgrade) {
            c.close();
        }
        true
    }

    /// Close, and wait for a running accept handler.
    pub fn release(&self) {
        self.close();
        drop(self.callback_lock.lock());
    }

    fn offer_waiting(&self) {
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

impl IoTarget for Listener {
    fn on_io(&self, op: OpRef, _bytes: u32, err: u32) {
        if op.kind() == K_OFFER {
            self.offer_waiting();
            return;
        }
        let conn = {
            let mut st = lock(&self.state);
            let Some(pos) = st.pending.iter().position(|i| op.is(&i.op)) else {
                return;
            };
            let mut inst = st.pending.remove(pos);
            if st.closed || (err != 0 && err != ERROR_PIPE_CONNECTED) {
                // Dropping the instance closes its handle, if it still has one.
                drop(inst);
                self.refill_locked(&mut st);
                return;
            }
            st.connected += 1;
            let h = inst.h;
            inst.h = INVALID_HANDLE_VALUE; // the connection owns it now
            let conn = PipeConn::new(
                self.ctx.clone(),
                h,
                inst.skip,
                &self.opts,
                self.opts.max_message_size,
            );
            st.conns.retain(|w| w.strong_count() > 0);
            st.conns.push(Arc::downgrade(&conn));
            self.refill_locked(&mut st);
            conn
        };
        self.ctx.add_connection(Arc::downgrade(&conn));
        conn.begin_server_handshake(self.me.clone(), self.opts.max_message_size);
    }
}
