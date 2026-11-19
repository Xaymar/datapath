// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Glue between Rust closures and the C callbacks.
//!
//! Closures are boxed and handed to the library as `user` pointers together
//! with a drop shim as `user_free`. Panics are caught at the boundary: they
//! must not unwind into the library.

use core::ffi::c_void;
use std::cell::Cell;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::mpsc::{channel, Sender};
use std::sync::{Mutex, OnceLock};

thread_local! {
    /// Callbacks (of any kind) currently running on this thread.
    static IN_CALLBACK: Cell<u32> = const { Cell::new(0) };
    /// `user_free` shims currently running on this thread.
    static IN_FREE: Cell<u32> = const { Cell::new(0) };
}

struct Depth(&'static std::thread::LocalKey<Cell<u32>>);
impl Depth {
    fn enter(key: &'static std::thread::LocalKey<Cell<u32>>) -> Self {
        key.with(|d| d.set(d.get() + 1));
        Depth(key)
    }
}
impl Drop for Depth {
    fn drop(&mut self) {
        self.0.with(|d| d.set(d.get() - 1));
    }
}

/// Run user code from a callback: track it and keep panics on this side.
pub(crate) fn guard<R>(fallback: R, f: impl FnOnce() -> R) -> R {
    let _d = Depth::enter(&IN_CALLBACK);
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(r) => r,
        Err(_) => {
            eprintln!("datapath: a callback panicked; the panic was caught at the FFI boundary");
            fallback
        }
    }
}

pub(crate) fn in_callback() -> bool {
    IN_CALLBACK.with(|d| d.get() > 0)
}

pub(crate) fn in_free() -> bool {
    IN_FREE.with(|d| d.get() > 0)
}

/// `user_free` for a `Box<T>`.
pub(crate) unsafe extern "C" fn drop_box<T>(user: *mut c_void) {
    let _d = Depth::enter(&IN_FREE);
    let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(user as *mut T))));
}

pub(crate) fn into_user<T>(value: T) -> *mut c_void {
    Box::into_raw(Box::new(value)) as *mut c_void
}

/// A release call that may not run on the current thread (for example from a
/// `user_free`, which must not call back into DataPath).
pub(crate) struct Deferred(pub(crate) Box<dyn FnOnce() + Send>);

/// Run `release` on a helper thread. Only used for drops that happen inside
/// library callbacks, so it is off every hot path.
pub(crate) fn release_later(release: Deferred) {
    static REAPER: OnceLock<Mutex<Sender<Deferred>>> = OnceLock::new();
    let tx = REAPER.get_or_init(|| {
        let (tx, rx) = channel::<Deferred>();
        std::thread::Builder::new()
            .name("datapath-reaper".into())
            .spawn(move || {
                for job in rx {
                    (job.0)();
                }
            })
            .expect("datapath: failed to start the reaper thread");
        Mutex::new(tx)
    });
    let tx = tx.lock().unwrap_or_else(|e| e.into_inner());
    if let Err(e) = tx.send(release) {
        // The reaper is gone (process shutting down): leak rather than deadlock.
        core::mem::forget(e.0);
    }
}

/// A raw handle moved to the reaper thread.
pub(crate) struct SendPtr<T>(pub(crate) *mut T);
// SAFETY: only used to hand a handle we exclusively own to another thread.
unsafe impl<T> Send for SendPtr<T> {}
