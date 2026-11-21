// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! The functions of `datapath.h`.

use datapath_core::{
    Context, ContextOptions, Error, IdealCore, Options, Permissions, Priority, SendError, Server,
    Socket,
};
use std::ffi::{c_char, c_int, c_void, CStr};
use std::io::IoSlice;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::slice;
use std::time::Duration;

// Errors -------------------------------------------------------------------

pub type datapath_error = i32;

pub const DATAPATH_ERROR_UNKNOWN: i32 = -1;
pub const DATAPATH_SUCCESS: i32 = 0;
pub const DATAPATH_ERROR_FAILURE: i32 = 1;
pub const DATAPATH_ERROR_CRITICAL_FAILURE: i32 = 2;
pub const DATAPATH_ERROR_CLOSED: i32 = 3;
pub const DATAPATH_ERROR_TIMED_OUT: i32 = 4;
pub const DATAPATH_ERROR_INVALID_PATH: i32 = 5;
pub const DATAPATH_ERROR_NOT_SUPPORTED: i32 = 6;
pub const DATAPATH_ERROR_TOO_LARGE: i32 = 7;
pub const DATAPATH_ERROR_WOULD_BLOCK: i32 = 8;
pub const DATAPATH_ERROR_VERSION_MISMATCH: i32 = 9;
pub const DATAPATH_ERROR_NOT_FOUND: i32 = 10;
pub const DATAPATH_ERROR_ADDRESS_IN_USE: i32 = 11;
pub const DATAPATH_ERROR_ACCESS_DENIED: i32 = 12;
pub const DATAPATH_ERROR_INVALID_ARGUMENT: i32 = 13;
pub const DATAPATH_ERROR_OUT_OF_MEMORY: i32 = 14;

pub const DATAPATH_PERMISSIONS_USER: u32 = 0;
pub const DATAPATH_PERMISSIONS_GROUP: u32 = 1;
pub const DATAPATH_PERMISSIONS_WORLD: u32 = 2;

/// Version of this C interface (`DATAPATH_C_ABI_VERSION` in the header).
pub const DATAPATH_C_ABI_VERSION: u32 = 1;

fn code(e: Error) -> datapath_error {
    e.code()
}

fn send_code(r: Result<(), SendError>) -> datapath_error {
    match r {
        Ok(()) => DATAPATH_SUCCESS,
        Err(e) => e.to_error().code(),
    }
}

/// Runs `f`; a panic becomes `CRITICAL_FAILURE` instead of unwinding into C.
fn guarded(f: impl FnOnce() -> datapath_error) -> datapath_error {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(DATAPATH_ERROR_CRITICAL_FAILURE)
}

#[no_mangle]
pub extern "C" fn datapath_error_string(error: datapath_error) -> *const c_char {
    let s: &'static [u8] = match error {
        -1 => b"Unknown\0",
        0 => b"Success\0",
        1 => b"Failure\0",
        2 => b"CriticalFailure\0",
        3 => b"Closed\0",
        4 => b"TimedOut\0",
        5 => b"InvalidPath\0",
        6 => b"NotSupported\0",
        7 => b"TooLarge\0",
        8 => b"WouldBlock\0",
        9 => b"VersionMismatch\0",
        10 => b"NotFound\0",
        11 => b"AddressInUse\0",
        12 => b"AccessDenied\0",
        13 => b"InvalidArgument\0",
        14 => b"OutOfMemory\0",
        _ => b"Invalid\0",
    };
    s.as_ptr().cast()
}

// Version ------------------------------------------------------------------

/// `(abi << 16) | patch`, the same encoding as the C++ library.
#[no_mangle]
pub extern "C" fn datapath_version() -> u32 {
    let major: u32 = env!("CARGO_PKG_VERSION_MAJOR").parse().unwrap_or(0);
    let patch: u32 = env!("CARGO_PKG_VERSION_PATCH").parse().unwrap_or(0);
    (major << 16) | patch
}

#[no_mangle]
pub extern "C" fn datapath_c_abi_version() -> u32 {
    DATAPATH_C_ABI_VERSION
}

// Handles ------------------------------------------------------------------

pub struct datapath_context {
    ctx: Context,
}

pub struct datapath_socket {
    socket: Socket,
}

pub struct datapath_server {
    server: Server,
}

// Options ------------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy)]
pub struct datapath_context_options {
    pub struct_size: u32,
    pub priority: i32,
    pub spin_us: i64,
    pub io_threads: u64,
    pub ideal_core: i32,
    pub mmcss: u8,
    pub alertable: u8,
    pub r#yield: u8,
    pub reserved0: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
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
}

fn context_options_defaults() -> datapath_context_options {
    let d = ContextOptions::default();
    datapath_context_options {
        struct_size: std::mem::size_of::<datapath_context_options>() as u32,
        priority: d.priority.level(),
        spin_us: d.spin.as_micros() as i64,
        io_threads: d.io_threads as u64,
        ideal_core: match d.ideal_core {
            IdealCore::Auto => -1,
            IdealCore::Off => -2,
            IdealCore::Core(n) => n as i32,
        },
        mmcss: d.mmcss as u8,
        alertable: d.alertable as u8,
        r#yield: d.yield_cpu as u8,
        reserved0: 0,
    }
}

fn options_defaults() -> datapath_options {
    let d = Options::default();
    datapath_options {
        struct_size: std::mem::size_of::<datapath_options>() as u32,
        permissions: d.permissions as u32,
        max_message_size: d.max_message_size,
        max_queued_bytes: d.max_queued_bytes,
        backlog: d.backlog as u64,
        max_clients: d.max_clients as u64,
        connect_timeout_ms: d.connect_timeout.as_millis() as i64,
        receive_buffer: d.receive_buffer as u64,
        pipe_buffer: d.pipe_buffer as u64,
    }
}

/// Copy a caller's option struct over the defaults, reading only the bytes the
/// caller says it has (older, smaller structs keep defaults for newer fields).
/// `None` if the struct is too small to even hold its size.
unsafe fn read_options<T: Copy>(defaults: T, input: *const T) -> Option<T> {
    if input.is_null() {
        return Some(defaults);
    }
    let declared = ptr::read_unaligned(input.cast::<u32>()) as usize;
    if declared < std::mem::size_of::<u32>() {
        return None;
    }
    let mut out = defaults;
    let n = declared.min(std::mem::size_of::<T>());
    ptr::copy_nonoverlapping(input.cast::<u8>(), (&mut out as *mut T).cast::<u8>(), n);
    // struct_size is the first field of both structs.
    ptr::write(
        (&mut out as *mut T).cast::<u32>(),
        std::mem::size_of::<T>() as u32,
    );
    Some(out)
}

unsafe fn convert_context_options(
    input: *const datapath_context_options,
) -> Option<ContextOptions> {
    let o = read_options(context_options_defaults(), input)?;
    if o.spin_us < 0 || !(0..=3).contains(&o.priority) || o.ideal_core < -2 {
        return None;
    }
    Some(ContextOptions {
        priority: Priority::from_level(o.priority)?,
        spin: Duration::from_micros(o.spin_us as u64),
        io_threads: o.io_threads as usize,
        ideal_core: match o.ideal_core {
            -1 => IdealCore::Auto,
            -2 => IdealCore::Off,
            n => IdealCore::Core(n as u32),
        },
        mmcss: o.mmcss != 0,
        alertable: o.alertable != 0,
        yield_cpu: o.r#yield != 0,
    })
}

unsafe fn convert_options(input: *const datapath_options) -> Option<Options> {
    let o = read_options(options_defaults(), input)?;
    if o.permissions > DATAPATH_PERMISSIONS_WORLD || o.connect_timeout_ms < 0 {
        return None;
    }
    Some(Options {
        permissions: match o.permissions {
            0 => Permissions::User,
            1 => Permissions::Group,
            _ => Permissions::World,
        },
        max_message_size: o.max_message_size,
        max_queued_bytes: o.max_queued_bytes,
        backlog: o.backlog as usize,
        max_clients: o.max_clients as usize,
        connect_timeout: Duration::from_millis(o.connect_timeout_ms as u64),
        receive_buffer: o.receive_buffer as usize,
        pipe_buffer: o.pipe_buffer as usize,
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_context_options_init(options: *mut datapath_context_options) {
    if !options.is_null() {
        ptr::write(options, context_options_defaults());
    }
}

#[no_mangle]
pub unsafe extern "C" fn datapath_options_init(options: *mut datapath_options) {
    if !options.is_null() {
        ptr::write(options, options_defaults());
    }
}

// Context ------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn datapath_context_create(
    options: *const datapath_context_options,
    out_context: *mut *mut datapath_context,
) -> datapath_error {
    if out_context.is_null() {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    let Some(opts) = convert_context_options(options) else {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    };
    guarded(|| match Context::builder().options(opts).build() {
        Ok(ctx) => {
            *out_context = Box::into_raw(Box::new(datapath_context { ctx }));
            DATAPATH_SUCCESS
        }
        Err(e) => code(e),
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_context_get_default(
    out_context: *mut *mut datapath_context,
) -> datapath_error {
    if out_context.is_null() {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    guarded(|| match Context::global() {
        Ok(ctx) => {
            *out_context = Box::into_raw(Box::new(datapath_context { ctx }));
            DATAPATH_SUCCESS
        }
        Err(e) => code(e),
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_context_release(context: *mut datapath_context) {
    if !context.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(context))));
    }
}

unsafe fn context_of(context: *mut datapath_context) -> Result<Context, Error> {
    if context.is_null() {
        Context::global()
    } else {
        Ok((*context).ctx.clone())
    }
}

// Callbacks ----------------------------------------------------------------

pub type datapath_message_fn = Option<
    unsafe extern "C" fn(
        user: *mut c_void,
        socket: *mut datapath_socket,
        data: *const u8,
        size: usize,
    ),
>;
pub type datapath_close_fn = Option<
    unsafe extern "C" fn(user: *mut c_void, socket: *mut datapath_socket, reason: datapath_error),
>;
pub type datapath_free_fn = Option<unsafe extern "C" fn(user: *mut c_void)>;
pub type datapath_accept_fn = Option<
    unsafe extern "C" fn(
        user: *mut c_void,
        server: *mut datapath_server,
        client: *mut datapath_socket,
    ) -> c_int,
>;

/// Owns a callback's user data; frees it when the callback is dropped.
struct UserData {
    user: *mut c_void,
    free: datapath_free_fn,
}

// The caller promises the user data may be used from the I/O threads.
unsafe impl Send for UserData {}

impl UserData {
    fn ptr(&self) -> *mut c_void {
        self.user
    }
}

impl Drop for UserData {
    fn drop(&mut self) {
        if let Some(f) = self.free {
            // `user_free` must not call back into DataPath or unwind.
            unsafe { f(self.user) };
        }
    }
}

/// A raw handle pointer that may move between threads.
struct HandlePtr<T>(*mut T);
impl<T> Clone for HandlePtr<T> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<T> Copy for HandlePtr<T> {}
unsafe impl<T> Send for HandlePtr<T> {}
impl<T> HandlePtr<T> {
    fn get(self) -> *mut T {
        self.0
    }
}

// Socket -------------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy)]
pub struct datapath_buffer {
    pub data: *const c_void,
    pub size: usize,
}

unsafe fn path_of<'a>(path: *const c_char) -> Option<&'a str> {
    CStr::from_ptr(path).to_str().ok()
}

#[no_mangle]
pub unsafe extern "C" fn datapath_connect(
    path: *const c_char,
    options: *const datapath_options,
    context: *mut datapath_context,
    out_socket: *mut *mut datapath_socket,
) -> datapath_error {
    if path.is_null() || out_socket.is_null() {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    let Some(opts) = convert_options(options) else {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    };
    let Some(path) = path_of(path) else {
        return DATAPATH_ERROR_INVALID_PATH;
    };
    guarded(|| {
        let ctx = match context_of(context) {
            Ok(c) => c,
            Err(e) => return code(e),
        };
        match Socket::connect(path).options(opts).context(&ctx).open() {
            Ok(socket) => {
                *out_socket = Box::into_raw(Box::new(datapath_socket { socket }));
                DATAPATH_SUCCESS
            }
            Err(e) => code(e),
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_socket_set_on_message(
    socket: *mut datapath_socket,
    callback: datapath_message_fn,
    user: *mut c_void,
    user_free: datapath_free_fn,
) -> datapath_error {
    let ud = UserData {
        user,
        free: user_free,
    };
    if socket.is_null() {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    guarded(|| {
        let handle = HandlePtr(socket);
        let Some(cb) = callback else {
            (*socket).socket.clear_on_message();
            return DATAPATH_SUCCESS;
        };
        (*socket).socket.set_on_message(move |data: &[u8]| {
            // SAFETY: the C side promised the callback and user data are valid
            // until `user_free` runs, which happens when this closure is dropped.
            unsafe { cb(ud.ptr(), handle.get(), data.as_ptr(), data.len()) }
        });
        DATAPATH_SUCCESS
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_socket_set_on_close(
    socket: *mut datapath_socket,
    callback: datapath_close_fn,
    user: *mut c_void,
    user_free: datapath_free_fn,
) -> datapath_error {
    let ud = UserData {
        user,
        free: user_free,
    };
    if socket.is_null() {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    guarded(|| {
        let handle = HandlePtr(socket);
        let Some(cb) = callback else {
            (*socket).socket.clear_on_close();
            return DATAPATH_SUCCESS;
        };
        (*socket).socket.set_on_close(move |reason| unsafe {
            cb(ud.ptr(), handle.get(), reason.to_error().code())
        });
        DATAPATH_SUCCESS
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_socket_send(
    socket: *mut datapath_socket,
    data: *const c_void,
    size: usize,
) -> datapath_error {
    if socket.is_null() || (data.is_null() && size != 0) {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    let bytes: &[u8] = if size == 0 {
        &[]
    } else {
        slice::from_raw_parts(data.cast::<u8>(), size)
    };
    guarded(|| send_code((*socket).socket.send(bytes)))
}

#[no_mangle]
pub unsafe extern "C" fn datapath_socket_sendv(
    socket: *mut datapath_socket,
    parts: *const datapath_buffer,
    count: usize,
) -> datapath_error {
    if socket.is_null() || (parts.is_null() && count != 0) {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    let parts: &[datapath_buffer] = if count == 0 {
        &[]
    } else {
        slice::from_raw_parts(parts, count)
    };
    for p in parts {
        if p.data.is_null() && p.size != 0 {
            return DATAPATH_ERROR_INVALID_ARGUMENT;
        }
    }
    let view = |p: &datapath_buffer| -> IoSlice<'_> {
        if p.size == 0 {
            IoSlice::new(&[])
        } else {
            IoSlice::new(slice::from_raw_parts(p.data.cast::<u8>(), p.size))
        }
    };
    guarded(|| {
        const INLINE: usize = 8;
        if count <= INLINE {
            let mut buf: [IoSlice<'_>; INLINE] = [
                IoSlice::new(&[]),
                IoSlice::new(&[]),
                IoSlice::new(&[]),
                IoSlice::new(&[]),
                IoSlice::new(&[]),
                IoSlice::new(&[]),
                IoSlice::new(&[]),
                IoSlice::new(&[]),
            ];
            for (slot, p) in buf.iter_mut().zip(parts) {
                *slot = view(p);
            }
            send_code((*socket).socket.send_vectored(&buf[..count]))
        } else {
            let all: Vec<IoSlice<'_>> = parts.iter().map(view).collect();
            send_code((*socket).socket.send_vectored(&all))
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_socket_close(socket: *mut datapath_socket) -> datapath_error {
    if socket.is_null() {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    guarded(|| {
        if (*socket).socket.close() {
            DATAPATH_SUCCESS
        } else {
            DATAPATH_ERROR_CLOSED
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_socket_good(socket: *const datapath_socket) -> c_int {
    if socket.is_null() {
        0
    } else {
        (*socket).socket.is_open() as c_int
    }
}

#[no_mangle]
pub unsafe extern "C" fn datapath_socket_max_message_size(socket: *const datapath_socket) -> u64 {
    if socket.is_null() {
        0
    } else {
        (*socket).socket.max_message_size()
    }
}

#[no_mangle]
pub unsafe extern "C" fn datapath_socket_release(socket: *mut datapath_socket) {
    if !socket.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(socket))));
    }
}

// Server -------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn datapath_host(
    path: *const c_char,
    options: *const datapath_options,
    context: *mut datapath_context,
    out_server: *mut *mut datapath_server,
) -> datapath_error {
    if path.is_null() || out_server.is_null() {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    let Some(opts) = convert_options(options) else {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    };
    let Some(path) = path_of(path) else {
        return DATAPATH_ERROR_INVALID_PATH;
    };
    guarded(|| {
        let ctx = match context_of(context) {
            Ok(c) => c,
            Err(e) => return code(e),
        };
        match Server::host(path).options(opts).context(&ctx).open() {
            Ok(server) => {
                *out_server = Box::into_raw(Box::new(datapath_server { server }));
                DATAPATH_SUCCESS
            }
            Err(e) => code(e),
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_server_set_on_accept(
    server: *mut datapath_server,
    callback: datapath_accept_fn,
    user: *mut c_void,
    user_free: datapath_free_fn,
) -> datapath_error {
    let ud = UserData {
        user,
        free: user_free,
    };
    if server.is_null() {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    guarded(|| {
        let srv = HandlePtr(server);
        let Some(cb) = callback else {
            (*server).server.clear_on_accept();
            return DATAPATH_SUCCESS;
        };
        (*server).server.set_on_accept(move |incoming| {
            // The client's handle exists before the decision: the callback may
            // set handlers and send on it. Only if it accepts does the
            // handshake complete; otherwise the client is released.
            let (socket, pending) = incoming.detach();
            let handle = Box::into_raw(Box::new(datapath_socket { socket }));
            // SAFETY: see the message callback.
            let keep = unsafe { cb(ud.ptr(), srv.get(), handle) } != 0;
            if keep {
                pending.finish();
            } else {
                drop(unsafe { Box::from_raw(handle) });
            }
        });
        DATAPATH_SUCCESS
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_server_close(server: *mut datapath_server) -> datapath_error {
    if server.is_null() {
        return DATAPATH_ERROR_INVALID_ARGUMENT;
    }
    guarded(|| {
        if (*server).server.close() {
            DATAPATH_SUCCESS
        } else {
            DATAPATH_ERROR_CLOSED
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn datapath_server_release(server: *mut datapath_server) {
    if !server.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(server))));
    }
}
