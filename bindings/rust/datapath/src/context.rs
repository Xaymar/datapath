// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

use core::ptr::NonNull;

use crate::callback::{in_callback, in_free, release_later, Deferred, SendPtr};
use crate::error::{check, Result};
use crate::ContextOptions;

/// Owns DataPath's I/O threads. Every socket and server runs on one context.
///
/// Dropping a context closes every socket and server on it, so keep it alive
/// for as long as they are in use (share it with `Arc` if needed). Calls that
/// take `Option<&Context>` use the process-wide default context for `None`.
pub struct Context {
    raw: NonNull<sys::datapath_context>,
}

// SAFETY: the C API is thread-safe for every call but release, which needs
// exclusive ownership (guaranteed by `Drop`).
unsafe impl Send for Context {}
unsafe impl Sync for Context {}

impl Context {
    /// Create a context with its own I/O threads.
    pub fn new(options: &ContextOptions) -> Result<Context> {
        let raw = options.to_raw();
        let mut out = core::ptr::null_mut();
        check(unsafe { sys::datapath_context_create(&raw, &mut out) })?;
        Ok(Context { raw: NonNull::new(out).expect("datapath_context_create returned NULL") })
    }

    /// A handle to the process-wide default context.
    pub fn global() -> Result<Context> {
        let mut out = core::ptr::null_mut();
        check(unsafe { sys::datapath_context_get_default(&mut out) })?;
        Ok(Context { raw: NonNull::new(out).expect("datapath_context_get_default returned NULL") })
    }

    pub(crate) fn as_ptr(&self) -> *mut sys::datapath_context {
        self.raw.as_ptr()
    }
}

pub(crate) fn raw_or_null(ctx: Option<&Context>) -> *mut sys::datapath_context {
    ctx.map_or(core::ptr::null_mut(), Context::as_ptr)
}

impl Drop for Context {
    fn drop(&mut self) {
        let raw = SendPtr(self.raw.as_ptr());
        // The last context handle joins the I/O threads: never do that from one.
        if in_callback() || in_free() {
            release_later(Deferred(Box::new(move || {
                let raw = raw;
                unsafe { sys::datapath_context_release(raw.0) }
            })));
        } else {
            unsafe { sys::datapath_context_release(raw.0) }
        }
    }
}

impl core::fmt::Debug for Context {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_tuple("Context").field(&self.raw).finish()
    }
}
