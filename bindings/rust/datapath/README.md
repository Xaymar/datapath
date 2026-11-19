# com-xaymar-datapath

Safe Rust bindings for [DataPath](https://github.com/Xaymar/datapath), a low
latency IPC library for local processes: named pipes on Windows, Unix domain
sockets on Linux and Android. It is message based, so every `send` arrives as
one message on the other end.

```toml
[dependencies]
datapath = { package = "com-xaymar-datapath", version = "2" }
```

```rust
use datapath::{Options, Server, Socket};
use std::sync::{Arc, Mutex};

// Echo server. The accept handler owns each new client socket.
let server = datapath::host("example")?;
let clients = Arc::new(Mutex::new(Vec::new()));
let keep = clients.clone();
server.on_accept(move |client: Socket| {
    client.on_message(|socket, data| { let _ = socket.send(data); }).ok();
    keep.lock().unwrap().push(client);
})?;

// Client.
let socket = datapath::connect("example")?;
socket.on_message(|_, data| println!("got {} bytes", data.len()))?;
socket.send(b"hello")?;
```

Things to know:

- Handlers run on DataPath's I/O threads. Keep them short. They get a
  `SocketRef` for their own socket, so they can reply without capturing it.
- `send` never blocks. It returns `Error::WouldBlock` when the send queue is
  full and `Error::TooLarge` above the negotiated message size limit.
- Dropping a `Socket` closes the connection, dropping a `Server` stops
  accepting, and dropping the last handle to a `Context` closes everything on
  it.
- Panics in handlers are caught at the FFI boundary and reported on stderr.
- `Options` and `ContextOptions` default to the library's own defaults.
  See `include/datapath/options.hpp` for what each setting costs or buys.

Building needs CMake 3.16+ and a C++17 compiler; see the `-sys` crate for
linking a prebuilt library instead.

Licensed under BSD-3-Clause.
