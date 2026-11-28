# com.xaymar.datapath
DataPath is an IPC library for fast and predictable messaging between processes on the same machine made in C++11 (or newer). Designed from the ground up to care about the worst case it is able to outperform or match existing solutions such as boost interprocess, libuv, Mojo and others. It allows for packets from a few bytes all the way to many megabytes between one server and any number of clients. The exact implementation varies by platform and makes use of the best measured available systems.

These are the official bindings for [DataPath](https://github.xaymar.com/datapath/)

## Features
- **Low Latency, High Bandwidth.** Heavily optimized for even the worst of the worst cases, achieving under 100µs even under near 100% system load for up to 4 KiB messages.
- **Thread-safe by design.** Messages are never interleaved and `send()` can be used from any thread without blocking.
- **No fragmentation.** Every `send()` message arrives as exactly one `recv()` result or `on_message()` call.
- **Cross-Platform by default.** Windows, Linux, macOS supported by design.
- **Access Control built in.** Set `User`, `Group` and `World` access to exactly what you need.
- **One API. One Standard.** The same API available on every platform and even in the official bindings.
- **Near Zero Dependencies.** Only the standard C++ library is required and a functional OS.
- **Built to KISS.** The API is kept as simple as possible so mistakes are much harder to make.

### Python specific Features
- **Prebuilt wheels.** CPython 3.10 and newer on Windows, Linux and macOS, x86-64 and ARM64. One abi3 wheel covers 3.12+, and free-threaded 3.14t/3.15t builds run without the GIL.
- **Interoperable with any DataPath client or server.** The same C++ library is included directly and linked statically so no separate protocol.
- **Type hints included.** The package ships `.pyi` stubs and `py.typed`.
- **Any bytes-like object.** `send()` takes `bytes`, `bytearray`, `memoryview`, NumPy arrays and anything else with a contiguous buffer.
- **Pythonic errors.** Every error derives from `DatapathError` and from the closest built-in exception, so `except ConnectionError:` or `except TimeoutError:` just work.

### Python specific Differences
* **Two ways to receive.** `recv()` and `accept()` are blocking calls for your own thread: the I/O thread copies each message into a queue without touching the interpreter, so a busy Python thread can never stall other connections. `on_message()` and `on_accept()` instead call your function directly on an I/O thread, which is close to the C++ latency but has to take the GIL first (up to `sys.getswitchinterval()` while another thread holds it, no wait on free-threaded Python). Setting `on_message` hands over already queued messages in order, and `on_message(None)` switches back to `recv()`.
* **Messages are copied.** Every message becomes a new `bytes` object, or is copied into your own buffer with `recv_into()`. The `recv()` queue is not bounded, so read it as fast as messages arrive.
* **The server accepts at once.** `host()` accepts every client right away and queues it for `accept()`, because a client's `connect()` only completes once the server accepted it. Pass `on_accept=` to `host()` to decide per client instead: returning `False` rejects it.
* **Dropping the last reference closes.** Sockets, servers and contexts close when garbage collected, so keep accepted sockets somewhere. All of them are context managers too.
* **Timeouts are in seconds.** `None` waits forever, and waits can be interrupted with Ctrl+C. Spin settings are in microseconds. Run `python bindings/python/benchmarks/pingpong.py local --help` on your machine for numbers that matter.

## Usage
```bash
pip install com.xaymar.datapath
```

```python
import datapath

# Server: echo every message.
clients = []
def on_accept(sock):
    clients.append(sock)                               # keep it, or the connection closes
    sock.on_message(lambda data: sock.send(data))      # runs on an I/O thread
    sock.on_close(lambda reason: clients.remove(sock))

server = datapath.host("my-service", on_accept=on_accept)

# Client.
with datapath.connect("my-service") as sock:
    sock.send(b"hello")                                # any bytes-like object
    sock.sendv([b"he", b"llo"])                        # one message from several parts, without joining them first
    print(sock.recv(timeout=1.0))                      # b'hello'
server.close()
```

### API
* `host(path, *, on_accept=None, **options) → Server` hosts a server. Raises a `DatapathError` such as `AddressInUseError`.
* `connect(path, *, connect_timeout=1.0, **options) → Socket` connects and returns after the handshake. Raises a `DatapathError` such as `NotFoundError` or `TimedOutError`.
* `Server` has `accept(timeout=None)`, `on_accept(callback)` and `close()`, which stops accepting; connected sockets stay open. `accept_spin_us` sets how long `accept()` busy-waits before it sleeps.
* `Socket` has `send(data)` and `sendv(parts)`, which never block and raise `WouldBlockError` when the send queue is full, `TooLargeError` or `ClosedError`. Receive with `recv(timeout=None)`, `recv_into(buffer, timeout=None)`, or `on_message(callback)`, and get told about the end with `on_close(callback)`. `close()` drops unsent messages; already received ones stay readable. Its properties are `good`, `closed`, `close_reason`, `max_message_size`, `pending` and `recv_spin_us`.
* `Context(spin_us=50, io_threads=1, priority=2, mmcss=False, ideal_core=-1, alertable=False, yield_=False, pair_peer=False)` creates separate I/O threads with their own tuning. Pass it as `context=`. Without one, everything uses the process-wide default context. Closing a context closes everything on it.
* Options map 1:1 to the C++ `datapath::options` in snake_case: `max_message_size`, `max_queued_bytes`, `receive_buffer`, `connect_timeout` (seconds, connect only), `permissions` (`Permissions.User`, `Group`, `World`), `backlog`, `max_clients`, `pipe_buffer` (host only).
* `DatapathError` has `error`, the native `datapath.Error` code. The subclasses are `ClosedError`, `TimedOutError`, `NotFoundError`, `AccessDeniedError`, `AddressInUseError`, `WouldBlockError`, `TooLargeError`, `InvalidPathError`, `NotSupportedError` and `VersionMismatchError`. Exceptions raised in callbacks go to `sys.unraisablehook`; one in `on_accept` rejects the client.

## License
BSD 3-Clause License, see `LICENSE`.
