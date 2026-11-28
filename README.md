# DataPath
**Fast, predictable message passing between processes on the same machine.**

DataPath is a C++ IPC library (C++11 or newer) for programs that talk to each other locally and care about the worst case, not just the average. It moves whole, length-framed messages, from a few bytes to many megabytes, between one server and any number of clients, with any number of threads sending at once, over each OS's native transport (named pipes and I/O completion ports on Windows, Unix domain sockets with epoll or kqueue elsewhere) or an opt-in shared-memory transport. Over shared memory, small messages (64 B to 1 KiB) make a round trip in 1–3 µs p99.9 on an idle machine and 5–15 µs with every core busy, measured at worst-case placement.

![Round-trip p99.9 latency on Linux: DataPath over shared memory vs libuv and Chromium Mojo](docs/images/latency-p999-linux.svg)

![Round-trip p99.9 latency on Windows: DataPath over shared memory vs libuv](docs/images/latency-p999-windows.svg)

## How it compares
Full round trip between two processes, p99.9 in µs, worst-case placement (client and server on different physical cores and, where the machine has them, different CCDs), median of 5 runs. Stress is one busy thread per logical CPU in a third process (at nice 12 on Linux); DataPath over shared memory under stress is the median of three invocations, other stress cells one.

**Linux**, AMD EPYC 7313P (8 cores, two CCDs), `--pin 1,5`:

| | 64 B idle | 4 KiB idle | 64 KiB idle | 1 MiB idle | 64 B stress | 4 KiB stress |
|---|---|---|---|---|---|---|
| **DataPath, shared memory** | **1.4** | **3.4** | **22** | **321** | **10** | **23** |
| DataPath, Unix socket | 13 | 19 | 60 | 479 | 249 | 5,507 |
| libuv + DataPath's spin/priority | 14 | 20 | 56 | 541 | 3,494 | 4,924 |
| libuv as shipped | 23 | 28 | 64 | 557 | 4,070 | 4,076 |
| Chromium Mojo, bindings (main thread) | 62 | 73 | 439 | 8,704 | 7,852 | 7,997 |

**Windows**, AMD Ryzen 9 7940HS (8 cores, one CCD), `--pin 8,12`:

| | 64 B idle | 4 KiB idle | 64 KiB idle | 1 MiB idle | 64 B stress | 4 KiB stress |
|---|---|---|---|---|---|---|
| **DataPath, shared memory** | **2.2** | **4.1** | **19** | **130** | **5.8** | **26** |
| DataPath, named pipe | 6.9 | 17 | 50 | 754 | 40 | 113 |
| libuv + DataPath's spin/priority | 15 | 22 | 74 | 9,659 | 104 | 187 |
| libuv as shipped | 31 | 35 | 66 | 1,075 | 265 | 401 |

Against [libuv](docs/comparisons/libuv.md), DataPath's socket and pipe paths win through scheduling policy (busy-poll and priority), and shared memory is a further 3–10× for small messages. Against [Chromium's Mojo](docs/comparisons/mojo.md) (Linux) it is 20–50× lower idle and 400–800× lower under full load for small messages. Full tables (p50, p99, p99.9), the methodology and the reasons are in [docs/comparisons](docs/comparisons/README.md).

## Features
* **Low tail latency.** No timers or sleeps on the data path: I/O threads busy-poll briefly (`spin`, 50 µs) after the last event, then sleep in the kernel, at raised priority. Zero-copy receive, several small messages per system call.
* **Shared memory, opt-in.** `options::shared_memory` moves messages through two rings mapped only by the two processes; the socket or pipe then only carries the handshake (Windows, Linux).
* **Whole messages.** Each `send()` arrives as one `on_message` call; messages from different threads never interleave. One server, many clients; `send()` is thread-safe and never blocks.
* **Explicit backpressure.** `send()` returns `TooLarge` or `WouldBlock` instead of blocking or growing without limit.
* **Safe setup and teardown.** Access control (`User`, `Group`, `World`), a version handshake, peer validation before `on_accept`; destroying a handle waits for running handlers.
* **One API** on Windows, Linux and macOS, with C, Python and Node.js bindings. No dependencies beyond the C++ standard library and the OS.

## Platforms
| Platform | Transport | Status |
|---|---|---|
| Windows 10 / 11 | Named pipes, IOCP; shared memory | Supported |
| Linux | Unix domain sockets, epoll; shared memory | Supported |
| macOS | Unix domain sockets, kqueue | Supported (not benchmarked yet) |
| Android, iOS | as Linux / macOS | Planned |

## Usage
```cpp
#include <datapath/datapath.hpp>

// Server
std::shared_ptr<datapath::server> server;
datapath::host(server, "my-service");          // \\.\pipe\my-service, or the abstract socket "datapath.my-service"
server->on_accept([&](std::shared_ptr<datapath::socket> client) {
    datapath::socket* raw = client.get();      // don't capture the shared_ptr in its own handler
    client->on_message([raw](const uint8_t* data, size_t size) {
        raw->send(data, size);                 // echo; send() is safe from handlers
    });
    clients.push_back(client);                 // keep it, or the connection closes
    return true;                               // false rejects; the client's connect() returns Closed
});

// Client
std::shared_ptr<datapath::socket> sock;
if (datapath::connect(sock, "my-service") == datapath::error::Success) {
    sock->on_message([](const uint8_t* data, size_t size) { /* data valid during the call */ });
    sock->send("hello", 5);
}
```
See `samples/echo/main.cpp` for a complete program.

### Semantics worth knowing
* **Threads.** Handlers run on the context's I/O threads; `on_message` for one socket never runs concurrently with itself.
* **Receiving.** Reads start when you first call `on_message`; data passed to it is valid during the call.
* **Backpressure.** `send()` returns `TooLarge` above `options::max_message_size` (64 MiB) and `WouldBlock` past `options::max_queued_bytes` (64 MiB). A message is queued whole or rejected.
* **Closing.** `close()` or dropping the last `shared_ptr` closes; it waits for a running handler, and no handler fires afterwards.
* **Contexts.** Sockets share one process-wide context by default; `datapath::context::create({spin, io_threads})` gives separate tuning.

Scheduling, placement, shared memory, buffers, tuning and paths per platform: [docs/semantics.md](docs/semantics.md).

### C API
`#include <datapath/datapath.h>` exposes the same features as a plain C ABI for C programs and other languages' FFI: opaque handles, `datapath_error` return codes, callbacks with a `void* user`. Built with `DATAPATH_SHARED=ON`, the library exports only the C API on Linux. The header documents threading and ownership rules.

## Getting DataPath
Add the repository as a subdirectory and link the `datapath::datapath` target:
```cmake
add_subdirectory(datapath)
target_link_libraries(my-app PRIVATE datapath::datapath)
```
Requires CMake 3.16+ and a C++11 compiler; MSVC needs C++14, its lowest mode. The library builds with C++17 by default; set `DATAPATH_CXX_STANDARD` (11, 14, 17, 20, 23) to pick another. To build the library, tests and benchmarks on their own, see [CONTRIBUTING.md](CONTRIBUTING.md).

### Python
```bash
pip install com.xaymar.datapath
```
```python
import datapath
with datapath.connect("my-service") as sock:
    sock.send(b"hello")
    print(sock.recv(timeout=1.0))
```
Wheels cover CPython 3.10+ (including free-threaded builds) on Windows and Linux. See [bindings/python/README.md](bindings/python/README.md) for the API, the two ways to receive and their latency.

### Node.js
```bash
npm install @xaymar/datapath
```
```js
const datapath = require('@xaymar/datapath');
const sock = await datapath.connect('my-service');
sock.on('message', (data) => console.log(data.toString()));
sock.send('hello');
```
A Node-API addon built on the C++ API, with TypeScript typings and prebuilt binaries for Windows and Linux, for Node.js 18+. See [bindings/node/README.md](bindings/node/README.md) for the API and how it differs from C++.

## Known issues (work in progress)
* No server-side handshake timeout: a local client that connects and never sends its hello holds a connection slot.

## License
BSD 3-Clause License, see `LICENSE`.
