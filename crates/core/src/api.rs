// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! The public API: contexts, sockets and servers.
//!
//! Handlers are given to a builder before any I/O starts, so no message can
//! arrive before its handler exists.
//!
//! ```no_run
//! use datapath_core::{Server, Socket};
//!
//! let server = Server::host("my-service")
//!     .on_accept(|incoming| {
//!         let socket = incoming
//!             .on_message(|msg| println!("got {} bytes", msg.len()))
//!             .accept();
//!         // Keep `socket` alive for as long as the connection should stay open.
//!         std::mem::forget(socket);
//!     })
//!     .open()?;
//!
//! let client = Socket::connect("my-service").open()?;
//! client.send(b"hello")?;
//! # drop(server);
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```

use crate::conn::{CloseHandler, CloseReason, MessageHandler, SendError};
use crate::error::Error;
use crate::options::{ContextOptions, IdealCore, Options, Permissions, Priority};
use crate::sys;
use std::io::IoSlice;
use std::sync::{Arc, OnceLock, Weak};
use std::time::Duration;

/// Owns the I/O threads. Cloning gives another handle; when the last handle is
/// dropped, every socket and server created on the context is closed (sockets
/// and servers themselves do not keep their context alive).
#[derive(Clone)]
pub struct Context {
    handle: Arc<sys::ContextHandle>,
}

impl Context {
    /// Start building a context.
    pub fn builder() -> ContextBuilder {
        ContextBuilder::default()
    }

    /// A context with the default options.
    pub fn new() -> Result<Context, Error> {
        Context::builder().build()
    }

    /// The process-wide default context, used when none is given.
    pub fn global() -> Result<Context, Error> {
        static GLOBAL: OnceLock<Context> = OnceLock::new();
        if let Some(c) = GLOBAL.get() {
            return Ok(c.clone());
        }
        let c = Context::new()?;
        Ok(GLOBAL.get_or_init(|| c).clone())
    }

    fn shared(&self) -> &Arc<sys::ContextShared> {
        self.handle.shared()
    }
}

/// Builds a [`Context`].
#[derive(Default)]
pub struct ContextBuilder {
    opts: ContextOptions,
}

impl ContextBuilder {
    /// Use these options as a whole.
    pub fn options(mut self, opts: ContextOptions) -> Self {
        self.opts = opts;
        self
    }

    /// I/O thread priority.
    pub fn priority(mut self, priority: Priority) -> Self {
        self.opts.priority = priority;
        self
    }

    /// Busy-poll time of an idle I/O thread before it blocks.
    pub fn spin(mut self, spin: Duration) -> Self {
        self.opts.spin = spin;
        self
    }

    /// Number of I/O threads; 0 = one per physical core.
    pub fn io_threads(mut self, n: usize) -> Self {
        self.opts.io_threads = n;
        self
    }

    /// Yield the CPU after inline writes and while spinning.
    pub fn yield_cpu(mut self, yes: bool) -> Self {
        self.opts.yield_cpu = yes;
        self
    }

    /// Windows only: core placement.
    pub fn ideal_core(mut self, core: IdealCore) -> Self {
        self.opts.ideal_core = core;
        self
    }

    /// Windows only: register the I/O threads with MMCSS.
    pub fn mmcss(mut self, yes: bool) -> Self {
        self.opts.mmcss = yes;
        self
    }

    /// Windows only: alertable waits.
    pub fn alertable(mut self, yes: bool) -> Self {
        self.opts.alertable = yes;
        self
    }

    /// Start the I/O threads.
    pub fn build(self) -> Result<Context, Error> {
        Ok(Context {
            handle: Arc::new(sys::ContextHandle::new(self.opts)?),
        })
    }
}

/// One end of a connection. Dropping it closes the connection; no handler runs
/// after the drop returns.
pub struct Socket {
    conn: Arc<sys::Conn>,
}

impl Socket {
    /// Connect to the server at `path`.
    pub fn connect(path: impl Into<String>) -> ConnectBuilder {
        ConnectBuilder {
            path: path.into(),
            context: None,
            opts: Options::default(),
            on_message: None,
            on_close: None,
        }
    }

    /// Queue one message. Never blocks; safe to call from handlers.
    pub fn send(&self, data: &[u8]) -> Result<(), SendError> {
        self.conn.send(&[data])
    }

    /// Queue one message assembled from several parts.
    pub fn send_vectored(&self, parts: &[IoSlice<'_>]) -> Result<(), SendError> {
        send_vectored(&self.conn, parts)
    }

    /// A cheap, cloneable handle that can only send. It does not keep the
    /// connection alive: once this socket is dropped, sending returns
    /// [`SendError::Closed`].
    pub fn sender(&self) -> Sender {
        Sender {
            conn: Arc::downgrade(&self.conn),
        }
    }

    /// Replace the message handler. The first handler starts receiving; until
    /// then the library does not read, so it also does not notice the peer closing.
    pub fn set_on_message(&self, handler: impl FnMut(&[u8]) + Send + 'static) {
        self.conn.set_message_handler(Some(Box::new(handler)));
    }

    /// Replace the close handler.
    pub fn set_on_close(&self, handler: impl FnOnce(CloseReason) + Send + 'static) {
        self.conn.set_close_handler(Some(Box::new(handler)));
    }

    /// Close the connection; the close handler fires.
    pub fn close(&self) {
        self.conn.close();
    }

    /// True while the connection is open.
    pub fn is_open(&self) -> bool {
        self.conn.is_open()
    }

    /// The negotiated message size limit of this connection.
    pub fn max_message_size(&self) -> u64 {
        self.conn.max_message_size()
    }
}

impl Drop for Socket {
    fn drop(&mut self) {
        self.conn.release();
    }
}

impl std::fmt::Debug for Socket {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Socket")
            .field("open", &self.is_open())
            .finish()
    }
}

fn send_vectored(conn: &sys::Conn, parts: &[IoSlice<'_>]) -> Result<(), SendError> {
    const INLINE: usize = 8;
    if parts.len() <= INLINE {
        let mut buf: [&[u8]; INLINE] = [&[]; INLINE];
        for (slot, p) in buf.iter_mut().zip(parts) {
            *slot = p;
        }
        conn.send(&buf[..parts.len()])
    } else {
        let all: Vec<&[u8]> = parts.iter().map(|p| &**p).collect();
        conn.send(&all)
    }
}

/// The sending half of a [`Socket`]; see [`Socket::sender`].
#[derive(Clone)]
pub struct Sender {
    conn: Weak<sys::Conn>,
}

impl Sender {
    /// Queue one message.
    pub fn send(&self, data: &[u8]) -> Result<(), SendError> {
        match self.conn.upgrade() {
            Some(c) => c.send(&[data]),
            None => Err(SendError::Closed),
        }
    }

    /// Queue one message assembled from several parts.
    pub fn send_vectored(&self, parts: &[IoSlice<'_>]) -> Result<(), SendError> {
        match self.conn.upgrade() {
            Some(c) => send_vectored(&c, parts),
            None => Err(SendError::Closed),
        }
    }
}

/// Builds a client [`Socket`].
pub struct ConnectBuilder {
    path: String,
    context: Option<Context>,
    opts: Options,
    on_message: Option<MessageHandler>,
    on_close: Option<CloseHandler>,
}

impl ConnectBuilder {
    /// Run on this context instead of the global one.
    pub fn context(mut self, context: &Context) -> Self {
        self.context = Some(context.clone());
        self
    }

    /// Use these options as a whole.
    pub fn options(mut self, opts: Options) -> Self {
        self.opts = opts;
        self
    }

    /// Largest message accepted.
    pub fn max_message_size(mut self, bytes: u64) -> Self {
        self.opts.max_message_size = bytes;
        self
    }

    /// How long to wait for the handshake.
    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.opts.connect_timeout = timeout;
        self
    }

    /// Handler for received messages. Runs on an I/O thread, never concurrently
    /// with itself or with the close handler of the same socket.
    pub fn on_message(mut self, handler: impl FnMut(&[u8]) + Send + 'static) -> Self {
        self.on_message = Some(Box::new(handler));
        self
    }

    /// Handler for the end of the connection; runs at most once.
    pub fn on_close(mut self, handler: impl FnOnce(CloseReason) + Send + 'static) -> Self {
        self.on_close = Some(Box::new(handler));
        self
    }

    /// Connect. Blocks until the handshake completes or the timeout passes.
    pub fn open(self) -> Result<Socket, Error> {
        let context = match self.context {
            Some(c) => c,
            None => Context::global()?,
        };
        let conn =
            context
                .shared()
                .connect(&self.path, &self.opts, self.on_message, self.on_close)?;
        Ok(Socket { conn })
    }
}

/// A hosted server. Dropping it stops accepting and closes its connections.
pub struct Server {
    listener: Arc<sys::Listener>,
}

impl Server {
    /// Host a server at `path`.
    pub fn host(path: impl Into<String>) -> HostBuilder {
        HostBuilder {
            path: path.into(),
            context: None,
            opts: Options::default(),
            on_accept: None,
        }
    }

    /// Stop accepting and close every connection of this server.
    pub fn close(&self) {
        self.listener.close();
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        self.listener.release();
    }
}

/// Builds a [`Server`].
pub struct HostBuilder {
    path: String,
    context: Option<Context>,
    opts: Options,
    on_accept: Option<Box<dyn FnMut(Incoming) + Send + 'static>>,
}

impl HostBuilder {
    /// Run on this context instead of the global one.
    pub fn context(mut self, context: &Context) -> Self {
        self.context = Some(context.clone());
        self
    }

    /// Use these options as a whole.
    pub fn options(mut self, opts: Options) -> Self {
        self.opts = opts;
        self
    }

    /// Who may connect.
    pub fn permissions(mut self, permissions: Permissions) -> Self {
        self.opts.permissions = permissions;
        self
    }

    /// Largest message accepted.
    pub fn max_message_size(mut self, bytes: u64) -> Self {
        self.opts.max_message_size = bytes;
        self
    }

    /// Maximum connected clients; 0 = unlimited.
    pub fn max_clients(mut self, n: usize) -> Self {
        self.opts.max_clients = n;
        self
    }

    /// Handler for clients that passed the handshake. Runs on an I/O thread.
    /// Accept an [`Incoming`] to get a [`Socket`]; drop it to reject. Clients
    /// that arrive before a handler is set wait.
    pub fn on_accept(mut self, handler: impl FnMut(Incoming) + Send + 'static) -> Self {
        self.on_accept = Some(Box::new(handler));
        self
    }

    /// Start hosting.
    pub fn open(self) -> Result<Server, Error> {
        let context = match self.context {
            Some(c) => c,
            None => Context::global()?,
        };
        let listener = context.shared().listen(&self.path, &self.opts)?;
        if let Some(mut f) = self.on_accept {
            listener
                .set_accept_handler(Some(Box::new(move |conn| f(Incoming { conn: Some(conn) }))));
        }
        Ok(Server { listener })
    }
}

/// A client that passed the handshake and waits for a decision: call
/// [`accept`](Incoming::accept) to get its [`Socket`], or drop it to reject (the
/// client's connect then fails with [`Error::Closed`]).
pub struct Incoming {
    conn: Option<Arc<sys::Conn>>,
}

impl Incoming {
    /// Set the message handler of the future socket.
    pub fn on_message(self, handler: impl FnMut(&[u8]) + Send + 'static) -> Self {
        if let Some(c) = &self.conn {
            c.set_message_handler(Some(Box::new(handler)));
        }
        self
    }

    /// Set the close handler of the future socket.
    pub fn on_close(self, handler: impl FnOnce(CloseReason) + Send + 'static) -> Self {
        if let Some(c) = &self.conn {
            c.set_close_handler(Some(Box::new(handler)));
        }
        self
    }

    /// Accept the client.
    pub fn accept(mut self) -> Socket {
        let conn = self.conn.take().expect("accept consumes the Incoming");
        conn.complete_accept();
        Socket { conn }
    }
}

impl Drop for Incoming {
    fn drop(&mut self) {
        if let Some(c) = self.conn.take() {
            c.close();
        }
    }
}
