// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Safe Rust bindings for [DataPath](https://github.com/Xaymar/datapath), a
//! low latency IPC library for local processes (named pipes on Windows, Unix
//! domain sockets on Linux and Android).
//!
//! DataPath is message based: every `send` arrives as one message on the other
//! end. Handlers run on DataPath's I/O threads, so keep them short and hand
//! heavier work to your own threads.
//!
//! ```no_run
//! use datapath::{Options, Server, Socket};
//!
//! # fn main() -> datapath::Result<()> {
//! // Echo server.
//! let server = Server::host("example", &Options::default(), None)?;
//! let clients = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
//! let keep = clients.clone();
//! server.on_accept(move |client: Socket| {
//!     client.on_message(|socket, data| { let _ = socket.send(data); }).ok();
//!     keep.lock().unwrap().push(client);
//! })?;
//!
//! // Client.
//! let socket = Socket::connect("example", &Options::default(), None)?;
//! socket.on_message(|_, data| println!("got {} bytes", data.len()))?;
//! socket.send(b"hello")?;
//! # Ok(()) }
//! ```
//!
//! The crate is published as `com-xaymar-datapath` and imported as `datapath`.

mod callback;
mod context;
mod error;
mod options;
mod server;
mod socket;

pub use context::Context;
pub use error::{Error, Result};
pub use options::{ContextOptions, IdealCore, Options, Permissions, Priority};
pub use server::Server;
pub use socket::{Socket, SocketRef};

/// The raw C API, for anything the safe wrappers do not cover.
pub use sys;

/// Version of the loaded library as `(major, minor, patch)`.
pub fn version() -> (u16, u16, u16) {
    let v = unsafe { sys::datapath_version() };
    ((v >> 24) as u16, ((v >> 8) & 0xffff) as u16, (v & 0xff) as u16)
}

/// Connect to `path` with default options on the default context.
pub fn connect(path: &str) -> Result<Socket> {
    Socket::connect(path, &Options::default(), None)
}

/// Host `path` with default options on the default context.
pub fn host(path: &str) -> Result<Server> {
    Server::host(path, &Options::default(), None)
}
