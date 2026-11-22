// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause


//! Raw bindings to the DataPath C API (`include/datapath/datapath.h`).
//!
//! Everything here is `unsafe` and mirrors the C header one to one; the
//! [`com-xaymar-datapath`](https://crates.io/crates/com-xaymar-datapath) crate
//! wraps it in a safe API. See the header for the full contract of each call.

#![no_std]
#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_void};

/// Version of the C interface these declarations match.
pub const DATAPATH_C_ABI_VERSION: u32 = 1;

// Errors ----------------------------------------------------------------------

pub type datapath_error = i32;

pub const DATAPATH_ERROR_UNKNOWN: datapath_error = -1;
pub const DATAPATH_SUCCESS: datapath_error = 0;
pub const DATAPATH_ERROR_FAILURE: datapath_error = 1;
pub const DATAPATH_ERROR_CRITICAL_FAILURE: datapath_error = 2;
pub const DATAPATH_ERROR_CLOSED: datapath_error = 3;
pub const DATAPATH_ERROR_TIMED_OUT: datapath_error = 4;
pub const DATAPATH_ERROR_INVALID_PATH: datapath_error = 5;
pub const DATAPATH_ERROR_NOT_SUPPORTED: datapath_error = 6;
pub const DATAPATH_ERROR_TOO_LARGE: datapath_error = 7;
pub const DATAPATH_ERROR_WOULD_BLOCK: datapath_error = 8;
pub const DATAPATH_ERROR_VERSION_MISMATCH: datapath_error = 9;
pub const DATAPATH_ERROR_NOT_FOUND: datapath_error = 10;
pub const DATAPATH_ERROR_ADDRESS_IN_USE: datapath_error = 11;
pub const DATAPATH_ERROR_ACCESS_DENIED: datapath_error = 12;
pub const DATAPATH_ERROR_INVALID_ARGUMENT: datapath_error = 13;
pub const DATAPATH_ERROR_OUT_OF_MEMORY: datapath_error = 14;

// Handles ---------------------------------------------------------------------

macro_rules! opaque {
    ($($name:ident),*) => {$(
        #[repr(C)]
        pub struct $name {
            _data: [u8; 0],
            // Not Send/Sync/Unpin on its own; the safe crate decides.
            _marker: core::marker::PhantomData<(*mut u8, core::marker::PhantomPinned)>,
        }
    )*};
}
opaque!(datapath_context, datapath_socket, datapath_server);

// Options ---------------------------------------------------------------------

pub const DATAPATH_PERMISSIONS_USER: u32 = 0;
pub const DATAPATH_PERMISSIONS_GROUP: u32 = 1;
pub const DATAPATH_PERMISSIONS_WORLD: u32 = 2;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct datapath_context_options {
    pub struct_size: u32,
    pub priority: i32,
    pub spin_us: i64,
    pub io_threads: u64,
    pub ideal_core: i32,
    pub mmcss: u8,
    pub alertable: u8,
    pub yield_: u8,
    pub pair_peer: u8,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct datapath_options {
    pub struct_size: u32,
    pub permissions: u32,
    pub max_message_size: u64,
    pub max_queued_bytes: u64,
    pub backlog: u64,
    pub max_clients: u64,
    pub connect_timeout_ms: i64,
    pub receive_buffer: u64,
    pub pipe_buffer: u64,
    pub shared_memory: u8,
    pub reserved: [u8; 7],
    pub shared_memory_size: u64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct datapath_buffer {
    pub data: *const c_void,
    pub size: usize,
}

// Callbacks -------------------------------------------------------------------

pub type datapath_message_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, socket: *mut datapath_socket, data: *const u8, size: usize)>;
pub type datapath_close_fn =
    Option<unsafe extern "C" fn(user: *mut c_void, socket: *mut datapath_socket, reason: datapath_error)>;
pub type datapath_free_fn = Option<unsafe extern "C" fn(user: *mut c_void)>;
pub type datapath_accept_fn = Option<
    unsafe extern "C" fn(user: *mut c_void, server: *mut datapath_server, client: *mut datapath_socket) -> c_int,
>;

extern "C" {
    pub fn datapath_error_string(error: datapath_error) -> *const c_char;

    pub fn datapath_version() -> u32;
    pub fn datapath_c_abi_version() -> u32;

    pub fn datapath_context_options_init(options: *mut datapath_context_options);
    pub fn datapath_options_init(options: *mut datapath_options);

    pub fn datapath_context_create(
        options: *const datapath_context_options,
        out_context: *mut *mut datapath_context,
    ) -> datapath_error;
    pub fn datapath_context_get_default(out_context: *mut *mut datapath_context) -> datapath_error;
    pub fn datapath_context_release(context: *mut datapath_context);

    pub fn datapath_connect(
        path: *const c_char,
        options: *const datapath_options,
        context: *mut datapath_context,
        out_socket: *mut *mut datapath_socket,
    ) -> datapath_error;
    pub fn datapath_socket_set_on_message(
        socket: *mut datapath_socket,
        callback: datapath_message_fn,
        user: *mut c_void,
        user_free: datapath_free_fn,
    ) -> datapath_error;
    pub fn datapath_socket_set_on_close(
        socket: *mut datapath_socket,
        callback: datapath_close_fn,
        user: *mut c_void,
        user_free: datapath_free_fn,
    ) -> datapath_error;
    pub fn datapath_socket_send(socket: *mut datapath_socket, data: *const c_void, size: usize) -> datapath_error;
    pub fn datapath_socket_sendv(
        socket: *mut datapath_socket,
        parts: *const datapath_buffer,
        count: usize,
    ) -> datapath_error;
    pub fn datapath_socket_close(socket: *mut datapath_socket) -> datapath_error;
    pub fn datapath_socket_good(socket: *const datapath_socket) -> c_int;
    pub fn datapath_socket_max_message_size(socket: *const datapath_socket) -> u64;
    pub fn datapath_socket_release(socket: *mut datapath_socket);

    pub fn datapath_host(
        path: *const c_char,
        options: *const datapath_options,
        context: *mut datapath_context,
        out_server: *mut *mut datapath_server,
    ) -> datapath_error;
    pub fn datapath_server_set_on_accept(
        server: *mut datapath_server,
        callback: datapath_accept_fn,
        user: *mut c_void,
        user_free: datapath_free_fn,
    ) -> datapath_error;
    pub fn datapath_server_close(server: *mut datapath_server) -> datapath_error;
    pub fn datapath_server_release(server: *mut datapath_server);
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::mem::{size_of, MaybeUninit};

    // The library writes its own sizeof into struct_size: compare layouts.
    #[test]
    fn option_struct_sizes_match_library() {
        unsafe {
            let mut c = MaybeUninit::<datapath_context_options>::zeroed();
            datapath_context_options_init(c.as_mut_ptr());
            assert_eq!(c.assume_init().struct_size as usize, size_of::<datapath_context_options>());

            let mut o = MaybeUninit::<datapath_options>::zeroed();
            datapath_options_init(o.as_mut_ptr());
            assert_eq!(o.assume_init().struct_size as usize, size_of::<datapath_options>());
        }
    }

    // Crate versions follow the library's ABI.patch: major = ABI, minor = patch.
    #[test]
    fn crate_version_matches_library() {
        let v = unsafe { datapath_version() };
        assert_eq!(v >> 16, env!("CARGO_PKG_VERSION_MAJOR").parse::<u32>().unwrap());
        assert_eq!(v & 0xffff, env!("CARGO_PKG_VERSION_MINOR").parse::<u32>().unwrap());
    }

    #[test]
    fn abi_version_matches() {
        assert_eq!(unsafe { datapath_c_abi_version() }, DATAPATH_C_ABI_VERSION);
    }
}
