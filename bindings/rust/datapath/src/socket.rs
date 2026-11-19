// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

use core::ffi::c_void;
use core::marker::PhantomData;
use core::ops::Deref;
use core::ptr::NonNull;
use std::ffi::CString;

use crate::callback::{drop_box, guard, in_free, into_user, release_later, Deferred, SendPtr};
use crate::context::raw_or_null;
use crate::error::{check, Error, Result};
use crate::{Context, Options};

/// Most parts `send_vectored` passes without allocating.
const STACK_PARTS: usize = 16;

/// The operations available on a socket, both through an owned [`Socket`]
/// (via `Deref`) and inside its callbacks, where the socket is borrowed for
/// the duration of the call.
pub struct SocketRef<'a> {
    raw: NonNull<sys::datapath_socket>,
    _life: PhantomData<&'a ()>,
}

// SAFETY: every call on SocketRef is thread-safe in the C API.
unsafe impl Send for SocketRef<'_> {}
unsafe impl Sync for SocketRef<'_> {}

impl SocketRef<'_> {
    /// Queue one message. Never blocks and is safe to call from callbacks.
    ///
    /// Fails with [`Error::TooLarge`], [`Error::WouldBlock`] (send queue full)
    /// or [`Error::Closed`] when the message was not queued.
    #[inline]
    pub fn send(&self, data: &[u8]) -> Result<()> {
        check(unsafe { sys::datapath_socket_send(self.raw.as_ptr(), data.as_ptr().cast(), data.len()) })
    }

    /// Queue one message assembled from several parts.
    pub fn send_vectored(&self, parts: &[&[u8]]) -> Result<()> {
        let empty = sys::datapath_buffer { data: core::ptr::null(), size: 0 };
        let to_buf = |p: &&[u8]| sys::datapath_buffer { data: p.as_ptr().cast(), size: p.len() };
        if parts.len() <= STACK_PARTS {
            let mut bufs = [empty; STACK_PARTS];
            for (b, p) in bufs.iter_mut().zip(parts) {
                *b = to_buf(p);
            }
            check(unsafe { sys::datapath_socket_sendv(self.raw.as_ptr(), bufs.as_ptr(), parts.len()) })
        } else {
            let bufs: Vec<_> = parts.iter().map(to_buf).collect();
            check(unsafe { sys::datapath_socket_sendv(self.raw.as_ptr(), bufs.as_ptr(), bufs.len()) })
        }
    }

    /// Close the connection. The close callback fires; the socket stays usable
    /// (every call returns [`Error::Closed`]) until dropped.
    pub fn close(&self) -> Result<()> {
        check(unsafe { sys::datapath_socket_close(self.raw.as_ptr()) })
    }

    /// Whether the connection is open.
    pub fn is_open(&self) -> bool {
        unsafe { sys::datapath_socket_good(self.raw.as_ptr()) != 0 }
    }

    /// Negotiated message size limit of this connection.
    pub fn max_message_size(&self) -> u64 {
        unsafe { sys::datapath_socket_max_message_size(self.raw.as_ptr()) }
    }
}

impl core::fmt::Debug for SocketRef<'_> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Socket").field("raw", &self.raw).field("open", &self.is_open()).finish()
    }
}

/// One end of a connection. Dropping it closes the connection.
///
/// Callbacks run on DataPath I/O threads. They receive a [`SocketRef`] for the
/// socket they belong to, so a handler can reply without capturing its own
/// `Socket` (which would keep the connection alive until it closes).
pub struct Socket {
    inner: SocketRef<'static>,
}

impl Socket {
    /// Connect to the server at `path`. Blocks until the handshake completes or
    /// `options.connect_timeout` elapses. `None` uses the default context.
    pub fn connect(path: &str, options: &Options, context: Option<&Context>) -> Result<Socket> {
        let path = to_c_path(path)?;
        let raw = options.to_raw();
        let mut out = core::ptr::null_mut();
        check(unsafe { sys::datapath_connect(path.as_ptr(), &raw, raw_or_null(context), &mut out) })?;
        Ok(unsafe { Socket::from_raw(out) })
    }

    /// SAFETY: `raw` is a valid socket handle that the caller owns.
    pub(crate) unsafe fn from_raw(raw: *mut sys::datapath_socket) -> Socket {
        Socket { inner: SocketRef { raw: NonNull::new(raw).expect("NULL socket handle"), _life: PhantomData } }
    }

    /// Set the message handler, replacing the previous one. The first handler
    /// starts receiving; messages that arrive earlier wait in the kernel.
    ///
    /// `data` is only valid during the call. Calls for one socket never overlap.
    pub fn on_message<F>(&self, handler: F) -> Result<()>
    where
        F: FnMut(&SocketRef<'_>, &[u8]) + Send + 'static,
    {
        unsafe extern "C" fn call<F: FnMut(&SocketRef<'_>, &[u8]) + Send + 'static>(
            user: *mut c_void,
            socket: *mut sys::datapath_socket,
            data: *const u8,
            size: usize,
        ) {
            // SAFETY: the library never runs message callbacks of one socket
            // concurrently, so this is the only live reference to the closure.
            let f = &mut *(user as *mut F);
            let s = SocketRef { raw: NonNull::new_unchecked(socket), _life: PhantomData };
            let data = if size == 0 { &[][..] } else { core::slice::from_raw_parts(data, size) };
            guard((), || f(&s, data));
        }
        check(unsafe {
            sys::datapath_socket_set_on_message(
                self.inner.raw.as_ptr(),
                Some(call::<F>),
                into_user(handler),
                Some(drop_box::<F>),
            )
        })
    }

    /// Remove the message handler.
    pub fn clear_on_message(&self) -> Result<()> {
        check(unsafe {
            sys::datapath_socket_set_on_message(self.inner.raw.as_ptr(), None, core::ptr::null_mut(), None)
        })
    }

    /// Set the handler called once when the connection ends, with the reason
    /// ([`Error::Closed`] for an orderly close). It does not run if the socket
    /// is dropped first.
    ///
    /// The peer closing is noticed by the receive path, which only runs once a
    /// message handler is set, so set [`on_message`](Self::on_message) as well.
    pub fn on_close<F>(&self, handler: F) -> Result<()>
    where
        F: FnOnce(&SocketRef<'_>, Error) + Send + 'static,
    {
        unsafe extern "C" fn call<F: FnOnce(&SocketRef<'_>, Error) + Send + 'static>(
            user: *mut c_void,
            socket: *mut sys::datapath_socket,
            reason: sys::datapath_error,
        ) {
            let slot = &mut *(user as *mut Option<F>);
            if let Some(f) = slot.take() {
                let s = SocketRef { raw: NonNull::new_unchecked(socket), _life: PhantomData };
                guard((), || f(&s, Error::from_code(reason)));
            }
        }
        check(unsafe {
            sys::datapath_socket_set_on_close(
                self.inner.raw.as_ptr(),
                Some(call::<F>),
                into_user(Some(handler)),
                Some(drop_box::<Option<F>>),
            )
        })
    }

    /// Remove the close handler.
    pub fn clear_on_close(&self) -> Result<()> {
        check(unsafe {
            sys::datapath_socket_set_on_close(self.inner.raw.as_ptr(), None, core::ptr::null_mut(), None)
        })
    }
}

impl Deref for Socket {
    type Target = SocketRef<'static>;
    fn deref(&self) -> &SocketRef<'static> {
        &self.inner
    }
}

impl core::fmt::Debug for Socket {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        self.inner.fmt(f)
    }
}

impl Drop for Socket {
    fn drop(&mut self) {
        let raw = SendPtr(self.inner.raw.as_ptr());
        // Releasing from a socket's own callbacks is fine, but not from a
        // user_free (a handler that owned a Socket being dropped).
        if in_free() {
            release_later(Deferred(Box::new(move || {
                let raw = raw;
                unsafe { sys::datapath_socket_release(raw.0) }
            })));
        } else {
            unsafe { sys::datapath_socket_release(raw.0) }
        }
    }
}

pub(crate) fn to_c_path(path: &str) -> Result<CString> {
    CString::new(path).map_err(|_| Error::InvalidPath)
}

