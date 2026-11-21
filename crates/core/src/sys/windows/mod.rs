// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Windows backend: named pipes with overlapped I/O on an I/O completion port.
//!
//! This is the only place in the crate with `unsafe` code: every call into the
//! Win32 API. Each unsafe block states what it relies on.

#![allow(unsafe_code)]

mod conn;
mod context;
mod listener;
mod path;
mod security;

pub(crate) use conn::PipeConn as Conn;
pub(crate) use context::{ContextHandle, ContextShared};
pub(crate) use listener::Listener;

use crate::conn::CloseReason;
use crate::error::Error;
use std::cell::UnsafeCell;
use std::sync::Arc;
use windows_sys::Win32::Foundation::{
    ERROR_ACCESS_DENIED, ERROR_BROKEN_PIPE, ERROR_FILE_NOT_FOUND, ERROR_INVALID_HANDLE,
    ERROR_NETNAME_DELETED, ERROR_NO_DATA, ERROR_OPERATION_ABORTED, ERROR_PATH_NOT_FOUND,
    ERROR_PIPE_NOT_CONNECTED, ERROR_SEM_TIMEOUT,
};
use windows_sys::Win32::System::IO::OVERLAPPED;

/// Map a Win32 error from a pipe operation to a DataPath error.
pub(crate) fn map_error(err: u32) -> Error {
    match err {
        ERROR_BROKEN_PIPE
        | ERROR_NO_DATA
        | ERROR_PIPE_NOT_CONNECTED
        | ERROR_OPERATION_ABORTED
        | ERROR_INVALID_HANDLE
        | ERROR_NETNAME_DELETED => Error::Closed,
        ERROR_FILE_NOT_FOUND | ERROR_PATH_NOT_FOUND => Error::NotFound,
        ERROR_ACCESS_DENIED => Error::AccessDenied,
        ERROR_SEM_TIMEOUT => Error::TimedOut,
        _ => Error::Failure,
    }
}

/// Why a connection ended, for a failed pipe operation.
pub(crate) fn reason_for(err: u32) -> CloseReason {
    match map_error(err) {
        Error::Closed => CloseReason::Remote,
        e => CloseReason::Io(e),
    }
}

/// Receives the completion of an overlapped operation. Runs on an I/O thread.
pub(crate) trait IoTarget: Send + Sync {
    fn on_io(&self, op: OpRef, bytes: u32, err: u32);
}

/// One overlapped operation. `ov` must stay first: the completion port hands
/// back a pointer to it, which is cast back to the whole `IoOp`.
#[repr(C)]
pub(crate) struct IoOp {
    ov: OVERLAPPED,
    kind: i32,
    /// Keeps the target alive until the completion has been dispatched; this
    /// is the library's own reference.
    keep: Option<Arc<dyn IoTarget>>,
}

/// An operation owned by a connection or server. Only the holder of the
/// matching slot (the writer, the read chain, ...) touches it while no
/// operation is pending, and the I/O thread that dispatches its completion
/// touches it afterwards, so accesses never overlap.
pub(crate) struct OpSlot(UnsafeCell<IoOp>);

// SAFETY: see above; the contents are plain data plus an `Arc` to a `Sync` target.
unsafe impl Send for OpSlot {}
unsafe impl Sync for OpSlot {}

impl OpSlot {
    pub fn new(kind: i32) -> OpSlot {
        OpSlot(UnsafeCell::new(IoOp {
            // SAFETY: OVERLAPPED is plain data; all zero is its idle state.
            ov: unsafe { std::mem::zeroed() },
            kind,
            keep: None,
        }))
    }

    /// Prepare the slot for a new operation and return its OVERLAPPED.
    ///
    /// # Safety
    /// No operation on this slot may be pending.
    pub unsafe fn arm(&self, keep: Arc<dyn IoTarget>) -> *mut OVERLAPPED {
        let op = &mut *self.0.get();
        op.ov = std::mem::zeroed();
        op.keep = Some(keep);
        &mut op.ov
    }

    /// Undo `arm` when the operation did not start, or finished at once.
    ///
    /// # Safety
    /// The operation must not produce a completion packet.
    pub unsafe fn disarm(&self) {
        (*self.0.get()).keep = None;
    }
}

/// A completed operation, as handed to [`IoTarget::on_io`].
#[derive(Clone, Copy)]
pub(crate) struct OpRef(pub(super) *mut IoOp);

impl OpRef {
    pub fn kind(self) -> i32 {
        // SAFETY: the slot outlives its dispatch (the target is kept alive
        // until `on_io` returns, and the slot lives inside the target).
        unsafe { (*self.0).kind }
    }

    pub fn is(self, slot: &OpSlot) -> bool {
        self.0 == slot.0.get()
    }
}
