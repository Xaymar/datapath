# DataPath for Python
**Fast, predictable message passing between processes on the same machine.**

Python bindings for [DataPath](https://github.com/Xaymar/datapath), a C++ IPC library built for low tail latency. It moves whole, length-framed messages between one server and any number of clients, over named pipes with I/O completion ports on Windows and Unix domain sockets with epoll on Linux.

```bash
pip install com.xaymar.datapath
```
The distribution is called `com.xaymar.datapath` (PyPI has no namespaces); the import name is `datapath`. Wheels are built for CPython 3.10 and newer (one abi3 wheel covers 3.12+, and free-threaded 3.14t/3.15t builds run without the GIL) on Windows, Linux and macOS, x86-64 and ARM64.

## Usage
```python
import datapath

# Server
with datapath.host("my-service") as server:
    client = server.accept()          # waits for the next client
    while True:
        data = client.recv()          # bytes; raises ClosedError when the client leaves
        client.send(data.upper())

# Client, in another process
with datapath.connect("my-service") as sock:
    sock.send(b"hello")
    print(sock.recv(timeout=1.0))     # b'HELLO'
```

Callbacks instead of `recv()`/`accept()`:
```python
clients = []

def on_accept(sock: datapath.Socket) -> None:
    clients.append(sock)                         # keep it, or the connection closes
    sock.on_message(lambda data: sock.send(data))  # runs on an I/O thread
    sock.on_close(lambda reason: clients.remove(sock))

server = datapath.host("my-service", on_accept=on_accept)
```

## Two ways to receive
* **`recv()` / `accept()`**: the I/O thread copies each message into a queue without touching the interpreter. `recv()` busy-waits for `Socket.recv_spin_us` (default 50 µs) with the GIL released, then sleeps until a message arrives. Use this when your own thread drives the conversation, or when other Python threads are busy: the I/O thread never waits for the GIL, so one slow Python thread can't stall other connections. The queue is bounded by `Socket.recv_high_water_mark` (default 1 MiB, each message counts its size plus 64 bytes): at the mark the socket stops reading, so a peer that sends faster than you call `recv()` gets `WouldBlockError` instead of growing your memory. `pending_bytes` shows the current fill, and `pause()` / `resume()` stop and restart reading on demand.
* **`on_message()` / `on_accept()`**: the I/O thread takes the GIL and calls your function. This skips the hand-off between threads and had the lowest latency in our measurements (below), close to the C++ library. The catch: when another Python thread holds the GIL, the I/O thread waits for it, up to `sys.getswitchinterval()` (5 ms by default) on GIL builds. On free-threaded Python there is no such wait.

Setting `on_message` hands over any messages already queued for `recv()`, in order; `on_message(None)` switches back. While a callback is set, `recv()` raises `RuntimeError`.

## Semantics worth knowing
* **Messages are whole.** Each `send()` arrives as one `recv()` result or callback, never split or merged. `send()` takes any contiguous bytes-like object (`bytes`, `bytearray`, `memoryview`, NumPy arrays…); `sendv([a, b, c])` sends several parts as one message. `recv_into(buffer)` copies into your own buffer.
* **`send()` never blocks.** It raises `TooLargeError` above the connection's `max_message_size` (default 64 MiB, the smaller of both peers' values), `WouldBlockError` when more than `max_queued_bytes` (default 64 MiB) is waiting, and `ClosedError` after the connection closed. It's thread-safe and can be called from callbacks.
* **Closing.** `close()` drops unsent messages. Messages already received stay readable: `recv()` returns them, then raises `ClosedError`. `close_reason` tells why the connection ended. A peer's disconnect is noticed while the socket is receiving, which starts with the first `recv()`, `on_message()` or `on_close()`. Dropping the last reference to a socket or server closes it too, so keep accepted sockets somewhere.
* **The server accepts at once.** A client's `connect()` only completes once the server accepted it, so `host()` accepts every client right away and queues it for `accept()`. Pass `on_accept=` to `host()` to decide per client: returning `False` rejects it, anything else (including `None`) accepts.
* **Errors** derive from `datapath.DatapathError` (with the native code in `.error`) and from the closest built-in: `ClosedError` is a `ConnectionError`, `TimedOutError` a `TimeoutError`, `NotFoundError` a `FileNotFoundError`, `AccessDeniedError` a `PermissionError`, `WouldBlockError` a `BlockingIOError`, `TooLargeError` and `InvalidPathError` a `ValueError`.
* **Exceptions in callbacks** go to `sys.unraisablehook` (printed to stderr by default). An exception in `on_accept` rejects the client.
* **Contexts.** All sockets share one process-wide set of I/O threads unless you pass `context=datapath.Context(spin_us=..., io_threads=..., priority=...)`. The options are those of the C++ `context_options`; see the [main README](https://github.com/Xaymar/datapath#semantics-worth-knowing). Closing a context closes everything on it.
* **Paths.** A bare name maps to `\\.\pipe\<name>` on Windows and to the abstract socket `datapath.<name>` on Linux; on Linux a path starting with `/` is a filesystem socket. Access is limited to the current user unless you pass `permissions=datapath.Permissions.Group` or `World`.
* **Timeouts** are in seconds (`None` waits forever); waits can be interrupted with Ctrl+C. Spin settings are in microseconds.

## Latency
Round trip (send, echo, receive) between two Python processes, from `python/benchmarks/pingpong.py`, median of 3 runs of 20,000 round trips. Measured on 2026-11-19 in a 4-core cloud container (Intel Xeon 2.1 GHz, Linux 6.18, CPython 3.10), so absolute numbers are higher and noisier than on a desktop. p99.9 in µs:

| | 8 B | 64 B | 4 KiB | 64 KiB |
|---|---|---|---|---|
| C++ client and server (`datapath-benchmark local`) | 43.5 | 30.0 | 71.1 | 252 |
| Python, callbacks on both sides | 45.2 | 36.8 | 45.9 | 75 |
| Python, `recv()` on both sides | 187 | 247 | 217 | 642 |

With four cores, `recv()` mode has four threads spinning at once (an I/O thread and a Python thread per process), which is the oversubscription the main README warns about; expect it to do better with more cores. Run `python python/benchmarks/pingpong.py local --help` to measure your own machine, including against the C++ echo server (`--cpp-server`).

## Building from source
```bash
pip install .            # from a checkout of the repository
pip install pytest && pytest python/tests
```
This needs CMake 3.16+ and a C++17 compiler; scikit-build-core and nanobind are fetched automatically.

## License
BSD 3-Clause License. Copyright (c) 2017 - 2026, Narta Xaymar Dirks.
