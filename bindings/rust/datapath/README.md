# com-xaymar-datapath
DataPath is an IPC library for fast and predictable messaging between processes on the same machine made in C++11 (or newer). Designed from the ground up to care about the worst case it is able to outperform or match existing solutions such as boost interprocess, libuv, Mojo and others. It allows for packets from a few bytes all the way to many megabytes between one server and any number of clients. The exact implementation varies by platform and makes use of the best measured available systems.

These are the official bindings for [DataPath](https://github.xaymar.com/datapath/)

## Features
- **Low Latency, High Bandwidth.** Heavily optimized for even the worst of the worst cases, achieving under 100µs even under near 100% system load for up to 4 KiB messages.
- **Thread-safe by design.** Messages are never interleaved and `send()` can be used from any thread without blocking.
- **No fragmentation.** Every `send()` message arrives as exactly one `on_message()` call.
- **Cross-Platform by default.** Windows, Linux, macOS supported by design.
- **Access Control built in.** Set `User`, `Group` and `World` access to exactly what you need.
- **One API. One Standard.** The same API available on every platform and even in the official bindings.
- **Near Zero Dependencies.** Only the standard C++ library is required and a functional OS.
- **Built to KISS.** The API is kept as simple as possible so mistakes are much harder to make.

### Rust specific Features
- **Safe by default.** `Socket`, `Server` and `Context` own their handles and close them on `Drop`. Handlers are `Send + 'static` closures, and the raw C API is still there as `datapath::sys` for anything the safe wrappers don't cover.
- **Panics never abort.** A panic in a handler is caught at the FFI boundary and reported on stderr, so it can't take down the I/O thread or your process.
- **Interoperable with any DataPath client or server.** The same C++ library is built by the [`com-xaymar-datapath-sys`](https://crates.io/crates/com-xaymar-datapath-sys) crate and linked statically so no separate protocol.

### Rust specific Differences
* **Handlers run on DataPath's I/O threads.** Just like in C++, so keep them short and hand heavier work to your own threads. Calls for one socket never overlap. A handler gets a `&SocketRef` for its own socket, so it can reply without capturing (and thereby keeping alive) its own `Socket`.
* **Message data is borrowed.** `on_message` gets a `&[u8]` that is only valid during the call, so nothing is copied unless you copy it.
* **Dropping closes.** Dropping a `Socket` closes the connection, dropping a `Server` stops accepting, and dropping the last handle to a `Context` closes everything on it. Keep accepted sockets somewhere.
* **Built from source.** The `-sys` crate compiles the bundled C++ sources with CMake 3.16+ and a C++17 compiler. To skip that, point `DATAPATH_LIB_DIR` at a prebuilt library from the [GitHub Releases](https://github.com/Xaymar/datapath/releases) for Windows, Linux or macOS.

## Usage
```bash
cargo add com-xaymar-datapath --rename datapath
```

```rust
use datapath::Socket;
use std::sync::{Arc, Mutex};

// Server: echo every message.
let server = datapath::host("my-service")?;
let clients = Arc::new(Mutex::new(Vec::<Socket>::new()));
let keep = clients.clone();
server.on_accept(move |client: Socket| {
    client.on_message(|sock, data| { let _ = sock.send(data); }).ok();
    client.on_close(|_, reason| println!("client left: {reason}")).ok();
    keep.lock().unwrap().push(client);                 // keep it, or the connection closes
})?;

// Client.
let sock = datapath::connect("my-service")?;
sock.on_message(|_, data| println!("{}", String::from_utf8_lossy(data)))?;  // "hello"
sock.send(b"hello")?;
sock.send_vectored(&[b"he".as_slice(), b"llo"])?;      // one message from several parts, without joining them first
sock.close()?;
server.close()?;
```

See `examples/echo.rs` for a complete program.

### API
* `host(path) → Result<Server>` and `Server::host(path, &options, context)` host a server. Fails with an `Error` such as `AddressInUse`.
* `connect(path) → Result<Socket>` and `Socket::connect(path, &options, context)` connect and return after the handshake. Fail with an `Error` such as `NotFound` or `TimedOut`.
* `Server` has `on_accept(handler)`, which owns each new `Socket`, `clear_on_accept()` and `close()`, which stops accepting; connected sockets stay open.
* `Socket` has `on_message(handler)` and `on_close(handler)`, which runs once with the reason (`Error::Closed` for an orderly close), plus `clear_on_message()` and `clear_on_close()`. Through `SocketRef` it has `send(data)` and `send_vectored(parts)`, which never block and return `Error::WouldBlock` when the send queue is full, `Error::TooLarge` or `Error::Closed`, plus `close()`, `is_open()` and `max_message_size()`.
* `Context::new(&ContextOptions)` creates separate I/O threads with their own tuning: `spin`, `io_threads`, `priority`, `ideal_core`, `mmcss`, `alertable`, `yield_cpu`, `pair_peer`. Pass it as `Some(&context)`. With `None`, everything uses the process-wide default context, also available as `Context::global()`.
* `Options` map 1:1 to the C++ `datapath::options`: `max_message_size`, `max_queued_bytes`, `receive_buffer`, `connect_timeout`, `permissions` (`Permissions::User`, `Group`, `World`), `backlog`, `max_clients`, `pipe_buffer`, `shared_memory`, `shared_memory_size`. `Default` gives the library's own defaults.
* `Error` is the `datapath::error` enum, with `code()` for its number and `as_str()` for its name. `version()` returns the loaded library's `(major, minor, patch)`.

## License
BSD 3-Clause License, see `LICENSE`.
