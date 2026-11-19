// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

use core::ffi::{c_int, c_void};
use core::ptr::NonNull;
use std::sync::Mutex;

use crate::callback::{drop_box, guard, in_callback, in_free, into_user, release_later, Deferred, SendPtr};
use crate::context::raw_or_null;
use crate::error::{check, Result};
use crate::socket::to_c_path;
use crate::{Context, Options, Socket};

/// A hosted server. Dropping it stops accepting; accepted sockets stay open.
pub struct Server {
    raw: NonNull<sys::datapath_server>,
}

// SAFETY: the C API is thread-safe for every call but release (see Drop).
unsafe impl Send for Server {}
unsafe impl Sync for Server {}

impl Server {
    /// Host a server at `path`. Set [`on_accept`](Self::on_accept) right away:
    /// clients wait until a handler is set. `None` uses the default context.
    pub fn host(path: &str, options: &Options, context: Option<&Context>) -> Result<Server> {
        let path = to_c_path(path)?;
        let raw = options.to_raw();
        let mut out = core::ptr::null_mut();
        check(unsafe { sys::datapath_host(path.as_ptr(), &raw, raw_or_null(context), &mut out) })?;
        Ok(Server { raw: NonNull::new(out).expect("datapath_host returned NULL") })
    }

    /// Set the handler for clients that passed the handshake. It runs on an
    /// I/O thread and owns the new [`Socket`]: keep it to keep the connection,
    /// drop it to close the connection again.
    pub fn on_accept<F>(&self, handler: F) -> Result<()>
    where
        F: FnMut(Socket) + Send + 'static,
    {
        unsafe extern "C" fn call<F: FnMut(Socket) + Send + 'static>(
            user: *mut c_void,
            _server: *mut sys::datapath_server,
            client: *mut sys::datapath_socket,
        ) -> c_int {
            let f = &*(user as *const Mutex<F>);
            // We own `client` from here on, whatever the handler does.
            let socket = Socket::from_raw(client);
            guard((), || {
                let mut f = f.lock().unwrap_or_else(|e| e.into_inner());
                (*f)(socket)
            });
            1
        }
        check(unsafe {
            sys::datapath_server_set_on_accept(
                self.raw.as_ptr(),
                Some(call::<F>),
                into_user(Mutex::new(handler)),
                Some(drop_box::<Mutex<F>>),
            )
        })
    }

    /// Remove the accept handler; new clients wait.
    pub fn clear_on_accept(&self) -> Result<()> {
        check(unsafe { sys::datapath_server_set_on_accept(self.raw.as_ptr(), None, core::ptr::null_mut(), None) })
    }

    /// Stop accepting. The server stays valid until dropped.
    pub fn close(&self) -> Result<()> {
        check(unsafe { sys::datapath_server_close(self.raw.as_ptr()) })
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        let raw = SendPtr(self.raw.as_ptr());
        // Release waits for a running accept callback, so never from inside one.
        if in_callback() || in_free() {
            release_later(Deferred(Box::new(move || {
                let raw = raw;
                unsafe { sys::datapath_server_release(raw.0) }
            })));
        } else {
            unsafe { sys::datapath_server_release(raw.0) }
        }
    }
}

impl core::fmt::Debug for Server {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_tuple("Server").field(&self.raw).finish()
    }
}
