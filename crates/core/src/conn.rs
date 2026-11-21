// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Platform-neutral part of a stream connection: handler storage, framing, the
//! send queue and the close-exactly-once state machine.
//!
//! Threading contract with a platform backend:
//!
//! * Messages are delivered only by the backend's read chain, which runs on I/O
//!   threads and is never concurrent with itself for one connection.
//! * [`ConnCore::finish_close`] is called either by the read chain (once it
//!   observes the connection closed) or, when reads were never armed, by a
//!   posted close event. `on_close` therefore never races with `on_message`.
//! * A handler that panics is caught here: the connection closes with
//!   [`CloseReason::HandlerPanic`] and the panic never reaches the backend.

use crate::error::Error;
use crate::proto::FrameParser;
use crate::queue::SendQueue;
use std::fmt;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};

/// Why a connection ended.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CloseReason {
    /// This side closed it (or dropped the socket).
    Local,
    /// The peer closed it in an orderly way.
    Remote,
    /// The peer broke the protocol (bad frame, message above the limit, ...).
    Protocol(Error),
    /// The operating system reported an I/O failure.
    Io(Error),
    /// A handler panicked.
    HandlerPanic,
}

impl CloseReason {
    /// The error code the C API reports for this reason.
    pub fn to_error(self) -> Error {
        match self {
            CloseReason::Local | CloseReason::Remote => Error::Closed,
            CloseReason::Protocol(e) | CloseReason::Io(e) => e,
            CloseReason::HandlerPanic => Error::Failure,
        }
    }
}

/// Why a message could not be queued.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SendError {
    /// The message is larger than the negotiated limit.
    TooLarge,
    /// The send queue is full; retry later.
    WouldBlock,
    /// The connection is closed.
    Closed,
    /// Any other failure (for example running out of memory).
    Other(Error),
}

impl SendError {
    /// The error code the C API reports.
    pub fn to_error(self) -> Error {
        match self {
            SendError::TooLarge => Error::TooLarge,
            SendError::WouldBlock => Error::WouldBlock,
            SendError::Closed => Error::Closed,
            SendError::Other(e) => e,
        }
    }
}

impl fmt::Display for SendError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.to_error().fmt(f)
    }
}

impl std::error::Error for SendError {}

/// Handler for a received message. Runs on an I/O thread.
pub type MessageHandler = Box<dyn FnMut(&[u8]) + Send + 'static>;
/// Handler for the end of a connection. Runs once, on an I/O thread.
pub type CloseHandler = Box<dyn FnOnce(CloseReason) + Send + 'static>;

pub(crate) fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// What a backend provides to [`ConnCore`].
pub(crate) trait Hooks {
    /// Release the OS handle so pending operations fail. Called once.
    fn platform_close(&self);
    /// Start the read chain on an I/O thread. Called once.
    fn platform_arm(&self);
    /// Arrange for `finish_close` to run on an I/O thread (reads never armed).
    fn platform_post_close(&self);
    /// The calling thread holds the send queue's writer slot: write until the
    /// queue is empty or the OS reports pending.
    fn platform_write(&self);
    /// Called after `send` wrote data from the caller's thread.
    fn after_inline_write(&self) {}
    /// Called after `on_close` has fired (server bookkeeping).
    fn on_finished(&self) {}
}

/// A mutex the same thread may take again. `release` takes it to wait for
/// running callbacks, and may be called from inside one.
pub(crate) struct ReentrantLock {
    /// (owner thread id, 0 = none; depth)
    state: Mutex<(u64, usize)>,
    cv: Condvar,
}

/// A cheap per-thread id (`thread::current().id()` clones an `Arc` each call).
fn thread_token() -> u64 {
    static NEXT: AtomicU64 = AtomicU64::new(1);
    thread_local! {
        static TOKEN: u64 = NEXT.fetch_add(1, Ordering::Relaxed);
    }
    TOKEN.with(|t| *t)
}

pub(crate) struct ReentrantGuard<'a>(&'a ReentrantLock);

impl ReentrantLock {
    pub(crate) fn new() -> Self {
        ReentrantLock {
            state: Mutex::new((0, 0)),
            cv: Condvar::new(),
        }
    }

    pub(crate) fn lock(&self) -> ReentrantGuard<'_> {
        let me = thread_token();
        let mut s = lock(&self.state);
        loop {
            if s.0 == 0 {
                *s = (me, 1);
                break;
            }
            if s.0 == me {
                s.1 += 1;
                break;
            }
            s = self.cv.wait(s).unwrap_or_else(|e| e.into_inner());
        }
        ReentrantGuard(self)
    }
}

impl Drop for ReentrantGuard<'_> {
    fn drop(&mut self) {
        let mut s = lock(&self.0.state);
        s.1 -= 1;
        if s.1 == 0 {
            s.0 = 0;
            self.0.cv.notify_one();
        }
    }
}

#[derive(Default)]
struct Handlers {
    on_message: Option<Arc<Mutex<MessageHandler>>>,
    on_close: Option<CloseHandler>,
}

pub(crate) struct ConnCore {
    handlers: Mutex<Handlers>,
    callback_lock: ReentrantLock,
    armed: AtomicBool,
    close_fired: AtomicBool,
    closed: AtomicBool,
    reason: Mutex<CloseReason>,
    /// Only the read chain (one I/O thread at a time) uses this.
    pub parser: Mutex<FrameParser>,
    pub queue: SendQueue,
}

impl ConnCore {
    pub fn new(negotiated_limit: u64, receive_buffer: usize, max_queued_bytes: u64) -> ConnCore {
        ConnCore {
            handlers: Mutex::new(Handlers::default()),
            callback_lock: ReentrantLock::new(),
            armed: AtomicBool::new(false),
            close_fired: AtomicBool::new(false),
            closed: AtomicBool::new(false),
            reason: Mutex::new(CloseReason::Local),
            parser: Mutex::new(FrameParser::new(negotiated_limit, receive_buffer)),
            queue: SendQueue::new(negotiated_limit, max_queued_bytes),
        }
    }

    pub fn armed(&self) -> bool {
        self.armed.load(Ordering::Acquire)
    }

    pub fn is_closed(&self) -> bool {
        self.closed.load(Ordering::Acquire)
    }

    /// Feed `n` freshly read bytes (read into `parser.prepare()`) to the parser.
    /// Returns false when the read chain must stop (and `finish_close` ran).
    pub fn consume<H: Hooks + ?Sized>(
        &self,
        parser: &mut FrameParser,
        n: usize,
        hooks: &H,
    ) -> bool {
        let _cb = self.callback_lock.lock();
        let handler = lock(&self.handlers).on_message.clone();
        let result = parser.commit(n, |data| {
            if let Some(h) = &handler {
                let mut f = lock(h);
                if catch_unwind(AssertUnwindSafe(|| (*f)(data))).is_err() {
                    self.close_with(CloseReason::HandlerPanic, hooks);
                }
            }
            !self.is_closed()
        });
        match result {
            Ok(()) if !self.is_closed() => return true,
            Ok(()) | Err(Error::Closed) => {}
            Err(e) => {
                self.close_with(CloseReason::Protocol(e), hooks);
            }
        }
        self.finish_close(hooks);
        false
    }

    /// Fire `on_close` once and drop the handlers (breaks reference cycles).
    pub fn finish_close<H: Hooks + ?Sized>(&self, hooks: &H) {
        let _cb = self.callback_lock.lock();
        if self.close_fired.swap(true, Ordering::AcqRel) {
            return;
        }
        let (on_message, on_close) = {
            let mut h = lock(&self.handlers);
            (h.on_message.take(), h.on_close.take())
        };
        // Handlers may own sockets: drop them outside the lock.
        drop(on_message);
        if let Some(f) = on_close {
            let reason = *lock(&self.reason);
            let _ = catch_unwind(AssertUnwindSafe(|| f(reason)));
        }
        hooks.on_finished();
    }

    /// Close once. Returns false if it was closed already.
    pub fn close_with<H: Hooks + ?Sized>(&self, reason: CloseReason, hooks: &H) -> bool {
        if self.closed.swap(true, Ordering::AcqRel) {
            return false;
        }
        *lock(&self.reason) = reason;
        self.queue.close();
        hooks.platform_close();
        if !self.armed() {
            hooks.platform_post_close();
        }
        true
    }

    /// Set or clear the message handler. The first handler starts the read chain.
    pub fn set_message_handler<H: Hooks + ?Sized>(
        &self,
        handler: Option<MessageHandler>,
        hooks: &H,
    ) {
        let has = handler.is_some();
        let old = {
            let mut h = lock(&self.handlers);
            if self.close_fired.load(Ordering::Acquire) {
                return;
            }
            std::mem::replace(&mut h.on_message, handler.map(|f| Arc::new(Mutex::new(f))))
        };
        drop(old);
        if has && !self.armed.swap(true, Ordering::AcqRel) {
            hooks.platform_arm();
        }
    }

    /// Store handlers before the connection is registered with a loop. Reads are
    /// considered armed: the backend registers for reading right away, so the
    /// first message cannot be missed and no arm notification is needed.
    pub fn install_handlers(
        &self,
        on_message: Option<MessageHandler>,
        on_close: Option<CloseHandler>,
    ) {
        let mut h = lock(&self.handlers);
        h.on_close = on_close;
        if let Some(f) = on_message {
            h.on_message = Some(Arc::new(Mutex::new(f)));
            self.armed.store(true, Ordering::Release);
        }
    }

    pub fn set_close_handler(&self, handler: Option<CloseHandler>) {
        let old = {
            let mut h = lock(&self.handlers);
            if self.close_fired.load(Ordering::Acquire) {
                return;
            }
            std::mem::replace(&mut h.on_close, handler)
        };
        drop(old);
    }

    pub fn send<H: Hooks + ?Sized>(&self, parts: &[&[u8]], hooks: &H) -> Result<(), SendError> {
        if self.is_closed() {
            return Err(SendError::Closed);
        }
        match self.queue.push(parts) {
            Ok(true) => {
                hooks.platform_write();
                hooks.after_inline_write();
                Ok(())
            }
            Ok(false) => Ok(()),
            Err(Error::TooLarge) => Err(SendError::TooLarge),
            Err(Error::WouldBlock) => Err(SendError::WouldBlock),
            Err(Error::Closed) => Err(SendError::Closed),
            Err(e) => Err(SendError::Other(e)),
        }
    }

    /// Close, wait for running callbacks, and drop the handlers without firing
    /// `on_close`. May be called from the connection's own callbacks.
    pub fn release<H: Hooks + ?Sized>(&self, hooks: &H) {
        self.close_with(CloseReason::Local, hooks);
        let _cb = self.callback_lock.lock();
        if self.close_fired.swap(true, Ordering::AcqRel) {
            return;
        }
        let (on_message, on_close) = {
            let mut h = lock(&self.handlers);
            (h.on_message.take(), h.on_close.take())
        };
        drop((on_message, on_close));
        hooks.on_finished();
    }
}
