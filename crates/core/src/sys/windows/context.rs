// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! A context: the I/O completion port, its I/O threads, and the connections and
//! servers living on them.

use super::conn::PipeConn;
use super::listener::Listener;
use super::path::make_pipe_path;
use super::{map_error, IoOp, IoTarget, OpSlot};
use crate::conn::{lock, CloseHandler, MessageHandler};
use crate::error::Error;
use crate::options::{ContextOptions, IdealCore, Options, Priority};
use crate::proto::{Hello, HELLO_SIZE};
use crate::spin::spin_then_block;
use std::cell::{Cell, RefCell};
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};
use std::sync::{Arc, Condvar, Mutex, Weak};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};
use windows_sys::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_IO_PENDING, ERROR_PIPE_BUSY, HANDLE, INVALID_HANDLE_VALUE,
    WAIT_OBJECT_0,
};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, ReadFile, SetFileCompletionNotificationModes, WriteFile, FILE_FLAG_OVERLAPPED,
    OPEN_EXISTING, SECURITY_IDENTIFICATION, SECURITY_SQOS_PRESENT,
};
use windows_sys::Win32::System::Kernel::PROCESSOR_NUMBER;
use windows_sys::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress, LoadLibraryW};
use windows_sys::Win32::System::Pipes::WaitNamedPipeW;
use windows_sys::Win32::System::SystemInformation::{
    GetLogicalProcessorInformationEx, RelationProcessorCore, GROUP_AFFINITY,
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
};
use windows_sys::Win32::System::Threading::{
    CreateEventW, GetCurrentThread, ResetEvent, SetThreadIdealProcessorEx, SetThreadPriority,
    WaitForSingleObject, THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST,
    THREAD_PRIORITY_TIME_CRITICAL,
};
use windows_sys::Win32::System::IO::{
    CancelIoEx, CreateIoCompletionPort, GetOverlappedResult, GetQueuedCompletionStatusEx,
    PostQueuedCompletionStatus, OVERLAPPED, OVERLAPPED_ENTRY,
};

const FILE_SKIP_COMPLETION_PORT_ON_SUCCESS: u8 = 0x1;
const FILE_SKIP_SET_EVENT_ON_HANDLE: u8 = 0x2;
const GENERIC_READ_WRITE: u32 = 0x8000_0000 | 0x4000_0000;

thread_local! {
    /// The context whose I/O thread is running on this thread, if any.
    static CURRENT_CONTEXT: Cell<usize> = const { Cell::new(0) };
}

/// True on any I/O thread of the context with this id.
fn on_io_thread_of(ctx_id: usize) -> bool {
    CURRENT_CONTEXT.with(|c| c.get() == ctx_id)
}

#[derive(Default)]
struct Members {
    connections: Vec<Weak<PipeConn>>,
    listeners: Vec<Weak<Listener>>,
}

pub(crate) struct ContextShared {
    opts: ContextOptions,
    port: HANDLE,
    threads: Mutex<Vec<JoinHandle<()>>>,
    stopped: AtomicBool,
    /// Operations that will still produce a completion packet.
    outstanding: AtomicI64,
    wait: Mutex<()>,
    wait_cv: Condvar,
    members: Mutex<Members>,
}

// SAFETY: the port handle is only used through thread-safe Win32 calls.
unsafe impl Send for ContextShared {}
unsafe impl Sync for ContextShared {}

/// The public handle: dropping the last one shuts the context down.
pub(crate) struct ContextHandle {
    shared: Arc<ContextShared>,
}

impl ContextHandle {
    pub fn new(opts: ContextOptions) -> Result<ContextHandle, Error> {
        Ok(ContextHandle {
            shared: ContextShared::create(opts)?,
        })
    }

    pub fn shared(&self) -> &Arc<ContextShared> {
        &self.shared
    }
}

impl Drop for ContextHandle {
    fn drop(&mut self) {
        self.shared.shutdown();
    }
}

/// First logical processor of every physical core, in system order. Called on
/// the thread that creates the context, never on an I/O thread.
fn physical_cores() -> Vec<PROCESSOR_NUMBER> {
    let mut out = Vec::new();
    // SAFETY: the buffer is sized by the first call and walked by the `Size`
    // field of each record, as the API documents. u64 words keep it aligned.
    unsafe {
        let mut len = 0u32;
        GetLogicalProcessorInformationEx(RelationProcessorCore, std::ptr::null_mut(), &mut len);
        if len == 0 {
            return out;
        }
        let mut buf = vec![0u64; (len as usize + 7) / 8];
        let base = buf.as_mut_ptr() as *mut u8;
        if GetLogicalProcessorInformationEx(RelationProcessorCore, base.cast(), &mut len) == 0 {
            return out;
        }
        let mut off = 0usize;
        while off < len as usize {
            let e = &*(base.add(off) as *const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
            if e.Relationship == RelationProcessorCore && e.Anonymous.Processor.GroupCount > 0 {
                let g: &GROUP_AFFINITY = &e.Anonymous.Processor.GroupMask[0];
                let mut mask = g.Mask;
                let mut bit = 0u8;
                while mask != 0 && mask & 1 == 0 {
                    mask >>= 1;
                    bit += 1;
                }
                if mask != 0 {
                    out.push(PROCESSOR_NUMBER {
                        Group: g.Group,
                        Number: bit,
                        Reserved: 0,
                    });
                }
            }
            if e.Size == 0 {
                break;
            }
            off += e.Size as usize;
        }
    }
    out
}

/// Core 0 is interrupt and DPC heavy: I/O threads start after it when there
/// are more than two cores.
fn first_io_core(cores: usize) -> usize {
    usize::from(cores > 2)
}

fn apply_ideal_core(opts: &ContextOptions, cores: &[PROCESSOR_NUMBER], index: usize) {
    if cores.is_empty() {
        return;
    }
    let pn = match opts.ideal_core {
        IdealCore::Off => return,
        IdealCore::Core(n) => cores[(n as usize + index) % cores.len()],
        IdealCore::Auto => {
            let first = first_io_core(cores.len());
            let span = cores.len() - first;
            let start = (std::process::id().wrapping_mul(2_654_435_761) as usize) % span;
            cores[first + (start + index) % span]
        }
    };
    // SAFETY: `pn` is a valid PROCESSOR_NUMBER; the old value is not requested.
    unsafe { SetThreadIdealProcessorEx(GetCurrentThread(), &pn, std::ptr::null_mut()) };
}

/// Look up an exported function; absent on older Windows versions.
///
/// # Safety
/// `F` must be the function pointer type matching the export.
unsafe fn proc<F: Copy>(module: HANDLE, name: &[u8]) -> Option<F> {
    let p = GetProcAddress(module, name.as_ptr())?;
    Some(std::mem::transmute_copy::<_, F>(&p))
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

/// Win11 may run background apps' threads in EcoQoS (efficiency cores, low
/// clocks). Opt the I/O threads out. Absent before Windows 10 1709: ignored.
fn disable_power_throttling() {
    #[repr(C)]
    struct ThrottlingState {
        version: u32,
        control_mask: u32,
        state_mask: u32,
    }
    type SetThreadInformation = unsafe extern "system" fn(HANDLE, i32, *const c_void, u32) -> i32;
    // SAFETY: kernel32 is always loaded; the signature is SetThreadInformation's.
    unsafe {
        let k32 = GetModuleHandleW(wide("kernel32.dll").as_ptr());
        if let Some(f) = proc::<SetThreadInformation>(k32, b"SetThreadInformation\0") {
            // THREAD_POWER_THROTTLING_CURRENT_VERSION, ..._EXECUTION_SPEED, off.
            let state = ThrottlingState {
                version: 1,
                control_mask: 1,
                state_mask: 0,
            };
            f(
                GetCurrentThread(),
                3, // ThreadPowerThrottling
                (&state as *const ThrottlingState).cast(),
                std::mem::size_of::<ThrottlingState>() as u32,
            );
        }
    }
}

/// Scheduling tweaks for an I/O thread. Returns the MMCSS handle to revert.
fn apply_thread_scheduling(opts: &ContextOptions) -> HANDLE {
    // SAFETY: plain Win32 calls on the current thread.
    unsafe {
        match opts.priority {
            Priority::Normal => {}
            Priority::Above => {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            }
            Priority::High => {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            }
            Priority::TimeCritical => {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            }
        }
        if !opts.mmcss {
            return 0;
        }
        type SetCharacteristics = unsafe extern "system" fn(*const u16, *mut u32) -> HANDLE;
        type SetPriority = unsafe extern "system" fn(HANDLE, i32) -> i32;
        let avrt = LoadLibraryW(wide("avrt.dll").as_ptr());
        if avrt == 0 {
            return 0;
        }
        let Some(set) = proc::<SetCharacteristics>(avrt, b"AvSetMmThreadCharacteristicsW\0") else {
            return 0;
        };
        let mut task = 0u32;
        let h = set(wide("Pro Audio").as_ptr(), &mut task);
        if h != 0 {
            if let Some(prio) = proc::<SetPriority>(avrt, b"AvSetMmThreadPriority\0") {
                prio(h, 2); // AVRT_PRIORITY_CRITICAL
            }
        }
        h
    }
}

fn revert_thread_scheduling(mmcss: HANDLE) {
    if mmcss == 0 {
        return;
    }
    type Revert = unsafe extern "system" fn(HANDLE) -> i32;
    // SAFETY: avrt.dll was loaded by `apply_thread_scheduling` on this thread.
    unsafe {
        let avrt = GetModuleHandleW(wide("avrt.dll").as_ptr());
        if avrt != 0 {
            if let Some(f) = proc::<Revert>(avrt, b"AvRevertMmThreadCharacteristics\0") {
                f(mmcss);
            }
        }
    }
}

impl ContextShared {
    pub fn create(mut opts: ContextOptions) -> Result<Arc<ContextShared>, Error> {
        let cores = if opts.io_threads == 0 || opts.ideal_core != IdealCore::Off {
            physical_cores()
        } else {
            Vec::new()
        };
        if opts.io_threads == 0 {
            // Automatic: one I/O thread per physical core that ideal_core would use.
            opts.io_threads = cores.len() - first_io_core(cores.len()).min(cores.len());
        }
        opts.io_threads = opts.io_threads.max(1);

        // SAFETY: creates a new, unassociated port.
        let port =
            unsafe { CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, opts.io_threads as u32) };
        if port == 0 {
            return Err(Error::CriticalFailure);
        }
        let shared = Arc::new(ContextShared {
            opts,
            port,
            threads: Mutex::new(Vec::new()),
            stopped: AtomicBool::new(false),
            outstanding: AtomicI64::new(0),
            wait: Mutex::new(()),
            wait_cv: Condvar::new(),
            members: Mutex::new(Members::default()),
        });
        // Each thread keeps its context alive, so the context is never
        // destroyed on one of its own I/O threads.
        for i in 0..shared.opts.io_threads {
            let keep = shared.clone();
            let cores = cores.clone();
            let spawned = std::thread::Builder::new()
                .name("datapath-io".into())
                .spawn(move || {
                    CURRENT_CONTEXT.with(|c| c.set(keep.id()));
                    apply_ideal_core(&keep.opts, &cores, i);
                    keep.run();
                });
            match spawned {
                Ok(h) => lock(&shared.threads).push(h),
                Err(_) => {
                    shared.shutdown();
                    return Err(Error::CriticalFailure);
                }
            }
        }
        Ok(shared)
    }

    /// Identifies this context on its I/O threads.
    pub fn id(&self) -> usize {
        self as *const ContextShared as usize
    }

    pub fn options(&self) -> &ContextOptions {
        &self.opts
    }

    pub fn stopped(&self) -> bool {
        self.stopped.load(Ordering::Acquire)
    }

    /// Bookkeeping for an operation that will produce a completion packet.
    pub fn op_started(&self) {
        self.outstanding.fetch_add(1, Ordering::Relaxed);
    }

    pub fn op_finished(&self) {
        if self.outstanding.fetch_sub(1, Ordering::AcqRel) == 1 && self.stopped() {
            let _l = lock(&self.wait);
            self.wait_cv.notify_all();
        }
    }

    /// Join the port and ask for completions to be skipped when an operation
    /// finishes at once. Returns whether that mode is on.
    pub fn associate(&self, h: HANDLE) -> Result<bool, Error> {
        // SAFETY: `h` is an open overlapped handle owned by the caller.
        unsafe {
            if CreateIoCompletionPort(h, self.port, 0, 0) == 0 {
                return Err(Error::CriticalFailure);
            }
            Ok(SetFileCompletionNotificationModes(
                h,
                FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE,
            ) != 0)
        }
    }

    /// Queue `op` for `target` as if it had completed (hops onto an I/O thread).
    pub fn post(&self, op: &OpSlot, target: Arc<dyn IoTarget>) -> bool {
        if self.stopped() {
            return false;
        }
        // SAFETY: callers post a slot only while no operation on it is pending.
        let ov = unsafe { op.arm(target) };
        self.op_started();
        // SAFETY: `ov` stays valid until the packet is dispatched (the target
        // owns the slot and is kept alive by the packet).
        if unsafe { PostQueuedCompletionStatus(self.port, 0, 0, ov) } == 0 {
            // SAFETY: no packet was queued.
            unsafe { op.disarm() };
            self.op_finished();
            return false;
        }
        true
    }

    pub fn add_connection(&self, c: Weak<PipeConn>) {
        let mut m = lock(&self.members);
        let n = m.connections.len();
        if n >= 64 && n & (n - 1) == 0 {
            m.connections.retain(|w| w.strong_count() > 0);
        }
        m.connections.push(c);
    }

    pub fn add_listener(&self, l: Weak<Listener>) {
        let mut m = lock(&self.members);
        m.listeners.retain(|w| w.strong_count() > 0);
        m.listeners.push(l);
    }

    fn dispatch(&self, entry: &OVERLAPPED_ENTRY) {
        let op = entry.lpOverlapped as *mut IoOp;
        // SAFETY: the pointer came from `OpSlot::arm`; the slot lives inside its
        // target, which `keep` holds alive until it is taken here. Everything
        // needed from the op is copied out before `on_io` may re-arm it.
        let (status, keep) = unsafe { ((*op).ov.Internal, (*op).keep.take()) };
        if let Some(target) = keep {
            target.on_io(
                super::OpRef(op),
                entry.dwNumberOfBytesTransferred,
                status_to_error(status as u32),
            );
        }
        self.op_finished();
    }

    fn pump(
        &self,
        timeout: u32,
        entries: &RefCell<Vec<OVERLAPPED_ENTRY>>,
        quit: &Cell<bool>,
    ) -> bool {
        let mut buf = entries.borrow_mut();
        let mut n = 0u32;
        // SAFETY: `buf` holds `len` entries; the port is open until shutdown joins us.
        let ok = unsafe {
            GetQueuedCompletionStatusEx(
                self.port,
                buf.as_mut_ptr(),
                buf.len() as u32,
                &mut n,
                timeout,
                i32::from(self.opts.alertable),
            )
        };
        if ok == 0 {
            return false;
        }
        for e in buf.iter().take(n as usize) {
            if e.lpOverlapped.is_null() {
                // One quit packet per thread: hand extras back to the others.
                if quit.get() {
                    // SAFETY: a null-overlapped packet is the quit signal.
                    unsafe { PostQueuedCompletionStatus(self.port, 0, 0, std::ptr::null()) };
                }
                quit.set(true);
                continue;
            }
            self.dispatch(e);
        }
        n > 0
    }

    fn run(&self) {
        disable_power_throttling();
        let mmcss = apply_thread_scheduling(&self.opts);
        // SAFETY: OVERLAPPED_ENTRY is plain data.
        let entries = RefCell::new(vec![unsafe { std::mem::zeroed::<OVERLAPPED_ENTRY>() }; 64]);
        let quit = Cell::new(false);
        let yield_cpu = self.opts.yield_cpu;
        while !quit.get() {
            spin_then_block(
                self.opts.spin,
                || !quit.get() && self.pump(0, &entries, &quit),
                || {
                    if !quit.get() {
                        self.pump(u32::MAX, &entries, &quit);
                    }
                },
                || {
                    if yield_cpu {
                        std::thread::yield_now();
                    } else {
                        crate::spin::cpu_relax();
                    }
                },
            );
        }
        revert_thread_scheduling(mmcss);
    }

    pub fn shutdown(self: &Arc<Self>) {
        if self.stopped.swap(true, Ordering::AcqRel) {
            return;
        }
        if on_io_thread_of(self.id()) {
            // An I/O thread cannot drain completions it is supposed to process,
            // nor join itself: finish on a separate thread.
            let me = self.clone();
            std::thread::spawn(move || me.shutdown_now());
            return;
        }
        self.shutdown_now();
    }

    fn shutdown_now(&self) {
        let (conns, listeners) = {
            let mut m = lock(&self.members);
            (
                std::mem::take(&mut m.connections),
                std::mem::take(&mut m.listeners),
            )
        };
        for l in listeners.iter().filter_map(Weak::upgrade) {
            l.close();
        }
        for c in conns.iter().filter_map(Weak::upgrade) {
            c.close();
        }
        // Let aborted operations drain (bounded), then stop the threads.
        {
            let mut guard = lock(&self.wait);
            let deadline = Instant::now() + Duration::from_secs(2);
            while self.outstanding.load(Ordering::Acquire) > 0 {
                let left = deadline.saturating_duration_since(Instant::now());
                if left.is_zero() {
                    break;
                }
                guard = self
                    .wait_cv
                    .wait_timeout(guard, left)
                    .unwrap_or_else(|e| e.into_inner())
                    .0;
            }
        }
        let threads = std::mem::take(&mut *lock(&self.threads));
        for _ in &threads {
            // SAFETY: a null-overlapped packet is the quit signal.
            unsafe { PostQueuedCompletionStatus(self.port, 0, 0, std::ptr::null()) };
        }
        for t in threads {
            let _ = t.join();
        }
    }

    /// Connect to the server at `path` and complete the handshake.
    pub fn connect(
        self: &Arc<Self>,
        path: &str,
        opts: &Options,
        on_message: Option<MessageHandler>,
        on_close: Option<CloseHandler>,
    ) -> Result<Arc<PipeConn>, Error> {
        if self.stopped() {
            return Err(Error::Closed);
        }
        let wpath = make_pipe_path(path).ok_or(Error::InvalidPath)?;
        let deadline = Instant::now() + opts.connect_timeout;
        let remaining_ms = || -> u32 {
            deadline
                .saturating_duration_since(Instant::now())
                .as_millis()
                .min(u32::MAX as u128 - 1) as u32
        };

        let h: HANDLE = loop {
            // SECURITY_IDENTIFICATION: the server may identify the client but
            // never act with its token (blocks named-pipe impersonation).
            // SAFETY: `wpath` is NUL-terminated.
            let h = unsafe {
                CreateFileW(
                    wpath.as_ptr(),
                    GENERIC_READ_WRITE,
                    0,
                    std::ptr::null(),
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                    0,
                )
            };
            if h != INVALID_HANDLE_VALUE {
                break h;
            }
            // SAFETY: reads the calling thread's last error.
            let err = unsafe { GetLastError() };
            if err != ERROR_PIPE_BUSY {
                return Err(map_error(err));
            }
            let left = remaining_ms();
            if left == 0 {
                return Err(Error::TimedOut);
            }
            // SAFETY: `wpath` is NUL-terminated.
            unsafe { WaitNamedPipeW(wpath.as_ptr(), left) };
        };

        // Handshake with event-based overlapped I/O before the handle joins the port.
        let handshake = || -> Result<Hello, Error> {
            let mut hello = Hello::new(opts.max_message_size).encode();
            transfer(h, true, &mut hello, &remaining_ms)?;
            let mut reply = [0u8; HELLO_SIZE];
            transfer(h, false, &mut reply, &remaining_ms)?;
            Hello::decode(&reply)
        };
        let reply = match handshake() {
            Ok(r) => r,
            Err(e) => {
                // SAFETY: `h` is ours and no operation is pending.
                unsafe { CloseHandle(h) };
                return Err(e);
            }
        };

        let skip = match self.associate(h) {
            Ok(s) => s,
            Err(e) => {
                // SAFETY: `h` is ours and no operation is pending.
                unsafe { CloseHandle(h) };
                return Err(e);
            }
        };
        let limit = opts.max_message_size.min(reply.max_message_size);
        let conn = PipeConn::new(self.clone(), h, skip, opts, limit);
        conn.install_handlers(on_message, on_close);
        self.add_connection(Arc::downgrade(&conn));
        conn.start_client();
        Ok(conn)
    }

    /// Host a server at `path`.
    pub fn listen(self: &Arc<Self>, path: &str, opts: &Options) -> Result<Arc<Listener>, Error> {
        if self.stopped() {
            return Err(Error::Closed);
        }
        let wpath = make_pipe_path(path).ok_or(Error::InvalidPath)?;
        let listener = Listener::new(self.clone(), wpath, opts.clone());
        if let Err(e) = listener.create() {
            listener.close();
            return Err(e);
        }
        self.add_listener(Arc::downgrade(&listener));
        Ok(listener)
    }
}

impl Drop for ContextShared {
    fn drop(&mut self) {
        // SAFETY: the threads are joined; nothing uses the port any more.
        unsafe { CloseHandle(self.port) };
    }
}

fn status_to_error(status: u32) -> u32 {
    if status == 0 {
        return 0;
    }
    // SAFETY: pure conversion function.
    unsafe { windows_sys::Win32::Foundation::RtlNtStatusToDosError(status as i32) }
}

/// Send or receive all of `buf` before the deadline `remaining_ms` counts down.
fn transfer(
    h: HANDLE,
    write: bool,
    buf: &mut [u8],
    remaining_ms: &dyn Fn() -> u32,
) -> Result<(), Error> {
    // SAFETY: a manual-reset event, closed below.
    let ev = unsafe { CreateEventW(std::ptr::null(), 1, 0, std::ptr::null()) };
    if ev == 0 {
        return Err(Error::CriticalFailure);
    }
    let result = (|| {
        let mut total = 0usize;
        while total < buf.len() {
            // SAFETY: OVERLAPPED is plain data.
            let mut ov: OVERLAPPED = unsafe { std::mem::zeroed() };
            ov.hEvent = ev;
            let mut got = 0u32;
            let n = (buf.len() - total) as u32;
            // SAFETY: `buf[total..]` and `ov` outlive the operation: it is
            // waited for (or cancelled and waited for) before leaving the loop.
            unsafe {
                ResetEvent(ev);
                let p = buf.as_mut_ptr().add(total);
                let ok = if write {
                    WriteFile(h, p, n, &mut got, &mut ov)
                } else {
                    ReadFile(h, p, n, &mut got, &mut ov)
                };
                if ok == 0 {
                    let err = GetLastError();
                    if err != ERROR_IO_PENDING {
                        return Err(map_error(err));
                    }
                    if WaitForSingleObject(ev, remaining_ms()) != WAIT_OBJECT_0 {
                        CancelIoEx(h, &ov);
                        GetOverlappedResult(h, &ov, &mut got, 1);
                        return Err(Error::TimedOut);
                    }
                    if GetOverlappedResult(h, &ov, &mut got, 0) == 0 {
                        return Err(map_error(GetLastError()));
                    }
                }
            }
            if got == 0 {
                return Err(Error::Closed);
            }
            total += got as usize;
        }
        Ok(())
    })();
    // SAFETY: closes the event created above.
    unsafe { CloseHandle(ev) };
    result
}
