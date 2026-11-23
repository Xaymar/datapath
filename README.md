# DataPath
**Fast, predictable message passing between processes on the same machine.**

DataPath is a C++17 IPC library for programs that talk to each other locally and care about the worst case, not just the average. It moves whole, length-framed messages, from a few bytes to many megabytes, between one server and any number of clients, with any number of threads sending at once. It uses each OS's native transport (named pipes with I/O completion ports on Windows, Unix domain sockets with epoll on Linux and kqueue on macOS) behind one small API, and keeps small-message round trips at 7–14 µs p99.9 on an idle machine, or 1–8 µs with its opt-in shared-memory transport.

![Round-trip p99.9 latency on Windows: DataPath over shared memory vs libuv](docs/images/latency-p999-windows.svg)

![Round-trip p99.9 latency on Linux: DataPath over shared memory vs libuv](docs/images/latency-p999-linux.svg)

Small messages are where DataPath is strongest. With its shared-memory transport (`options::shared_memory`, opt-in), a 64 B round trip on Windows has a p99.9 of 2.2 µs idle and 5.8 µs with every core busy, against 31 µs and 265 µs for libuv as shipped; on Linux 0.76 µs idle against 19 µs. Even libuv given DataPath's busy-poll and priority policy stays 7–18× higher there. The [benchmarks](#benchmarks) section has the full tables including DataPath's default pipe/socket transport, and what each configuration means.

## Features
**Latency**
* No timers or sleeps on the data path. An I/O thread busy-polls for a short, tunable time (`spin`, default 50 µs) after the last event, then sleeps in the kernel, so tail latency stays low under traffic and idle connections cost no CPU.
* Scheduling tuned for the tail: elevated I/O thread priority (thread priority on Windows, nice value on Linux where `RLIMIT_NICE` allows, quality-of-service class on macOS), opt-out of Windows EcoQoS throttling, and one I/O thread per physical core without sharing hyper-threads.
* Zero-copy receive: handlers get a pointer straight into the receive buffer, and several small messages arrive per system call.

**Messaging**
* Whole messages, never byte streams: each `send()` arrives as one `on_message` call, and messages from different threads never interleave.
* One server, many clients; `send()` is thread-safe and non-blocking from any thread, including from inside handlers.
* Bounded queues with explicit backpressure: `send()` returns `TooLarge` or `WouldBlock` instead of blocking or growing without limit.
* Large messages at about 9 GiB/s per direction on Windows and Linux.

**Safety and setup**
* Per-server access control (`User`, `Group`, `World`), a version handshake, and peer validation before `on_accept` runs.
* Clean shutdown: destroying a handle waits for running handlers, and no handler fires afterwards.
* One API on Windows, Linux and macOS, no dependencies outside the C++ standard library and the OS, usable as a CMake subdirectory.

## Platforms
| Platform | Transport | Status |
|---|---|---|
| Windows 10 / 11 | Named pipes, IOCP | Supported |
| Linux | Unix domain sockets, epoll | Supported |
| Android | Unix domain sockets, epoll | Planned (backend should apply) |
| macOS | Unix domain sockets, kqueue | Supported (not benchmarked yet) |
| iOS | Unix domain sockets, kqueue | Planned (backend should apply) |

## Benchmarks
Full round trip (send, echo, receive), ping-pong between two processes, release builds, median of 5 runs. Both machines were measured on 2026-11-23 at commit `df30376`, with DataPath using its shared-memory transport (`options::shared_memory`, opt-in; the benchmark sets `DATAPATH_SHARED_MEMORY=1`). The DataPath pipe/socket rows are the default transport, measured in the same session for reference. The benchmark ships a second transport over libuv 1.52.1 named pipes / Unix sockets with the same framing, clock, sizes and harness, so both libraries are measured the same way. How to reproduce these runs is in [CONTRIBUTING.md](CONTRIBUTING.md#benchmarking). The graphs above show each median with a whisker from the lowest to the highest p99.9 of the runs; `docs/images/latency_chart.py` redraws them.

* **DataPath**: defaults (`spin` 50 µs, `priority` 2, one I/O thread), plus `shared_memory` where it says so.
* **libuv as shipped**: `--spin 0 --no-mmcss --priority 0`, libuv's own loop.
* **libuv + policy**: libuv's loop thread given DataPath's spin and priority settings.
* **Placement (Windows)**: `--pin 8,12`, the client process on logical CPU 8 and the server on 12: two different physical cores, neither of them core 0. Without pinning, under stress each invocation's result depended on where the scheduler put the two I/O threads, for both libraries: on Irkela, 16 KiB p99.9 was about 15 µs with both on the two hyper-threads of one core, about 200 µs on separate cores, and about 1.1 ms when one of them was on core 0. Two separate cores is the case a typical client and server get. Linux runs are not pinned.
* **Stress**: `--stress 0`, one busy thread per logical core in a separate process. On Linux the stress process runs at nice 12 (`--stress-nice 12`), which is the gap `priority` 2 gives when the system lets the process raise its priority. The test container had `RLIMIT_NICE` 0, so the gap was created from the other side; at equal nice every variant measured 4–8 ms, one scheduler timeslice. DataPath over shared memory under stress was run three times (15 runs); the tables give the median of the three medians.

**Windows: Irkela**, AMD Ryzen 9 7940HS (8 cores / 16 threads), Windows 10 22H2, Balanced power plan, MSVC 19.51. p99.9 in µs:

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shared memory, idle | **8.2** | **2.2** | **2.1** | **2.6** | **4.1** | **8.6** | **18.9** | **42.3** | **130** |
| DataPath pipe, idle | 11.3 | 6.9 | 7.2 | 7.2 | 16.5 | 24.4 | 50.3 | 76.7 | 754 |
| libuv as shipped, idle | 31.0 | 30.7 | 29.8 | 30.6 | 34.5 | 44.7 | 65.8 | 278 | 1075 |
| libuv + policy, idle | 15.7 | 15.2 | 15.1 | 15.2 | 22.2 | 34.6 | 73.5 | 279 | 9659 |
| DataPath shared memory, stress | **5.1** | **5.8** | **7.2** | **15.3** | **25.6** | **74.3** | **528** | 3946 | **24394** |
| DataPath pipe, stress | 41.9 | 40.3 | 40.8 | 42.0 | 113 | 224 | 621 | **2096** | 28135 |
| libuv as shipped, stress | 244 | 265 | 282 | 293 | 401 | 1076 | 1600 | 36406 | 90842 |
| libuv + policy, stress | 105 | 104 | 114 | 111 | 187 | 273 | 653 | 7469 | 31434 |

**Linux: merenka-gpu0**, AMD EPYC 7313P container limited to 8 cores / 16 threads, Linux 6.12, GCC 15.2. p99.9 in µs:

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shared memory, idle | **0.78** | **0.76** | **0.83** | **0.99** | **1.2** | **2.6** | **8.1** | **68.3** | 331 |
| DataPath socket, idle | 10.5 | 10.4 | 10.2 | 14.7 | 20.9 | 28.2 | 50.2 | 74.7 | **248** |
| libuv as shipped, idle | 18.1 | 19.4 | 17.8 | 20.1 | 24.0 | 31.8 | 55.2 | 127 | 371 |
| libuv + policy, idle | 12.3 | 11.3 | 11.9 | 10.9 | 14.4 | 23.1 | 43.5 | 92.0 | 320 |
| DataPath shared memory, stress | **4.1** | **3.9** | **8.2** | **7.8** | **8.5** | **19.1** | **215** | **4220** | **6095** |
| DataPath socket, stress | 209 | 13.3 | 202 | 222 | 189 | 316 | 4098 | 8247 | 11991 |
| libuv as shipped, stress | 134 | 158 | 116 | 172 | 272 | 450 | 4158 | 8378 | 11366 |
| libuv + policy, stress | 214 | 20.6 | 113 | 180 | 255 | 131 | 795 | 8790 | 14117 |

What the numbers say:
* Over shared memory DataPath has the lowest tail at almost every size on both machines, idle and under stress. Idle it is 2–7× below libuv with the same policy on Windows up to 256 KiB (2.2 vs 15.2 µs at 64 B, 18.9 vs 73.5 µs at 64 KiB) and 9–16× below it on Linux up to 16 KiB (0.76 vs 11.3 µs at 64 B). At 64 KiB it moves 19 GiB/s on Windows and 32 GiB/s on Linux, against 6.3 and 3.7 GiB/s for libuv + policy.
* Under stress the gap is widest for small messages: 5–7 µs p99.9 up to 256 B on Windows against 104–114 µs for libuv + policy and 40–42 µs for DataPath's own pipe. On Linux it stays under 10 µs up to 4 KiB in the median run, where every socket-based variant measured 13–272 µs.
* Large messages under stress are where shared memory helps least. On Windows the pipe has the lower tail at 256 KiB (2.1 vs 2.3–4.0 ms over three repeats) and more throughput (735 vs 585 MiB/s); 64 KiB was stable at 446–529 µs and still lower than the pipe's 621. On Linux 256 KiB sat at 4.1–4.5 ms in all three repeats, about half of every socket variant. At 1 MiB every variant measures in milliseconds under full load.
* Idle at 1 MiB on Linux, the shared-memory tail is bimodal: runs measured either about 69 µs or about 335 µs, and the median (331 µs) is above the socket path (248 µs) and libuv + policy (320 µs), although p50 is 59 vs 210 µs.
* Run-to-run spread grows under stress, especially on Linux, where nothing is pinned: the three shared-memory repeats put 16 B at 0.95, 4.1 and 7.5 µs. On Windows (pinned) the three repeats agreed within about a third up to 64 KiB.
* Each size runs at most 2 GiB per run, so 256 KiB gets 8,192 round trips and 1 MiB only 2,048. There p99.9 is roughly the second or third slowest round trip, and a single hiccup moves it. One example in these runs: libuv + policy at 1 MiB idle on Irkela measured 5–16 ms p99.9 (1.2 ms in the previous README run).
* Single runs are noisy at microsecond resolution; compare medians of several runs (`--runs 5`).

DataPath p50 and p99.9 on the same machines, idle, in µs, shared memory against the default pipe/socket transport (Irkela pinned to 8,12 as above):

| | Irkela shm p50 | p99.9 | pipe p50 | p99.9 | merenka-gpu0 shm p50 | p99.9 | socket p50 | p99.9 |
|---|---|---|---|---|---|---|---|---|
| 16 B | 1.3 | 8.2 | 3.5 | 11.3 | 0.42 | 0.78 | 6.0 | 10.5 |
| 64 B | 0.6 | 2.2 | 3.2 | 6.9 | 0.42 | 0.76 | 6.2 | 10.4 |
| 256 B | 0.7 | 2.1 | 3.2 | 7.2 | 0.43 | 0.83 | 6.3 | 10.2 |
| 1 KiB | 0.8 | 2.6 | 3.1 | 7.2 | 0.45 | 0.99 | 7.1 | 14.7 |
| 4 KiB | 1.2 | 4.1 | 7.2 | 16.5 | 0.77 | 1.2 | 14.7 | 20.9 |
| 16 KiB | 2.4 | 8.6 | 11.3 | 24.4 | 1.3 | 2.6 | 19.8 | 28.2 |
| 64 KiB | 6.1 | 18.9 | 26.3 | 50.3 | 3.7 | 8.1 | 38.0 | 50.2 |
| 256 KiB | 20.4 | 42.3 | 50.8 | 76.7 | 45.5 | 68.3 | 57.2 | 74.7 |
| 1 MiB | 71.6 | 130 | 538 | 754 | 59.4 | 331 | 210 | 248 |

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
* **Threads.** Handlers run on the context's I/O threads. `on_message` for one socket never runs concurrently with itself. Messages from one sending thread arrive in order; messages from different threads arrive whole, never interleaved.
* **Receiving.** Reads start when you first call `on_message`. Until then, incoming data waits in the kernel.
* **Backpressure.** `send()` never blocks. It returns `TooLarge` above the connection's message size limit (`options::max_message_size`, default 64 MiB; the smaller of both peers' values applies). It returns `WouldBlock` when more than `options::max_queued_bytes` (default 64 MiB) is waiting. A message is either queued whole or rejected. Split very large payloads yourself.
* **Closing.** `close()` drops unsent messages and fires `on_close` once, on an I/O thread. Destroying your last `shared_ptr` to a socket or server closes it too. That waits for a handler that is currently running on another thread, and no handler of that object runs afterwards (`on_close` doesn't fire either). So it's safe to destroy what your handlers capture right after the handle.
* **Contexts.** All sockets share one process-wide context (I/O threads) by default. Create your own with `datapath::context::create({spin, io_threads})` and pass it to `connect`/`host` for separate tuning. Destroying a context closes everything on it.
* **Windows scheduling.** I/O threads run at `THREAD_PRIORITY_HIGHEST` (`context_options::priority`, 0–3). Under full CPU load, normal priority let single round trips stall for up to 12 ms; highest kept p99.9 under 100 µs. `context_options::mmcss` (registering with the Multimedia Class Scheduler) is off: on a busy system MMCSS throttles threads that use their CPU share, which measured as a fixed ~2 ms p99.9 stall, and on an idle system it made no difference. `yield` and `alertable` showed no consistent gain and are off.
* **Linux scheduling.** The same `context_options::priority` (default 2) lowers each I/O thread's nice value by 0, 4, 12 or 19 from the one it inherited. Unprivileged processes only get as far as `RLIMIT_NICE` allows, which is often not at all; `CAP_SYS_NICE` lifts the limit. Under full CPU load, a 12-step gap took 64 B p99.9 from about 8 ms to about 200 µs. No real-time scheduling policy is used.
* **macOS scheduling.** macOS picks threads, performance or efficiency cores and timer coalescing by quality-of-service class, so `priority` 1 sets `QOS_CLASS_USER_INITIATED` and 2 and 3 set `QOS_CLASS_USER_INTERACTIVE` (the class of a UI's main thread). No privileges are needed. Real-time (time constraint) threads are not used: the kernel demotes one that spins past its computation budget. `io_threads = 0` starts one I/O thread per physical core (`hw.physicalcpu`, which counts both core types on Apple silicon).
* **Core placement (Windows).** By default (`context_options::ideal_core = -1`) each I/O thread gets its own physical core as a soft hint (ideal processor, not hard affinity), never two on the hyper-threads of one core, and core 0, which handles most interrupts, is skipped when there are more than two cores. Core 0 is avoided with real thread affinity (within the process's own affinity), because under full CPU load Windows ignored the hint and ran I/O threads there, at about 1.1 ms instead of 0.2 ms p99.9 for 16 KiB. `context_options::pair_peer` (opt-in) goes further for a 1:1 link: when both the client's and the server's context set it (each with `io_threads = 1`), the handshake puts the two I/O threads on the two hyper-threads of one physical core, pinned. They then share that core's L1/L2 and no other thread runs there; under full CPU load (`--stress 0 --pair`, 4 unpinned runs, each landing on one core's two hyper-threads) 16 KiB p99.9 measured 13–14.5 µs and 64 KiB 22–33 µs, against 229 and 457 µs on separate cores, about the same as idle. 256 KiB still suffers under load (p99.9 0.57–2 ms vs 87 µs idle). Only the first such connection per context is paired, other connections on that context share the pinned core, and the pinning ends when that connection closes. A peer without the option, or with an older library, connects as before. A busy hyper-thread sibling roughly halves the speed of the other one; on the test machine this cut 64 KiB p99.9 from 72 to 52 µs and 8 B from 12 to 7 µs. `-2` leaves placement to the OS, `N` starts at physical core N. `io_threads = 0` starts exactly one I/O thread per physical core (on Windows, the cores this placement uses), so no two I/O threads ever share a core's hyper-threads.
* **Shared memory (Windows, Linux).** `options::shared_memory` (opt-in, both ends must set it) moves messages through two shared-memory rings (`options::shared_memory_size` per direction, default 4 MiB) instead of the pipe or socket, which then only carries the handshake and notices when the peer is gone. Only the two processes can map the rings. An idle receiver spins for `spin`, then waits on an event (Windows) or eventfd (Linux). A peer without the option, an older library or macOS connects as usual. `DATAPATH_SHARED_MEMORY=1` or `0` in the environment overrides the setting for every connection in a process. On Windows both processes must be in the same logon session, or the server must be allowed to create global objects. The [benchmarks](#benchmarks) compare it with the default transport.
* **Buffers.** `options::receive_buffer` (256 KiB) holds incoming frames; reads adapt their size to the traffic, so a large buffer doesn't slow down small messages. `options::pipe_buffer` (1 MiB) is the Windows kernel pipe buffer, and on macOS the kernel socket buffer of each connection (the macOS default is 8 KiB). Linux keeps its default socket buffers.
* **Tuning.** `context_options::spin` (default 50 µs) is how long an I/O thread busy-polls after the last event before sleeping in the kernel. Higher values cut tail latency for bursty traffic and cost CPU only while traffic flows. `0` disables spinning. Spinning only helps while spare CPUs exist: when spinning I/O threads cover every CPU (for example client and server on one machine, both with `io_threads = 0`), a woken thread waits for a spinner's budget to run out, and on Linux 64 B p99.9 measured about 116 µs with the default spin versus 23 µs with `0`. `context_options::yield` also fixes that case (24 µs) but gives a timeslice away on every yield under full CPU load. On Windows the I/O threads also opt out of EcoQoS power throttling.
* **Paths.** A bare name maps to `\\.\pipe\<name>` on Windows, to the abstract socket `datapath.<name>` on Linux, and to the socket file `/tmp/datapath.<name>` on macOS, which has no abstract namespace (`/tmp` rather than the per-user `$TMPDIR`, so other users can still reach a `Group` or `World` server). On Linux and macOS, a path starting with `/` is a filesystem socket, created with mode `0600`/`0660`/`0666` for `User`/`Group`/`World` (`Group` is the server's group) and removed on close. On macOS a server whose backlog is full refuses connections like a missing server does, so `connect()` keeps retrying while the socket file exists and returns `TimedOut` at `connect_timeout`.

### C API
`#include <datapath/datapath.h>` gives the same features as a plain C ABI for C programs and for other languages' FFI. Objects are opaque handles that you release yourself, every call returns a `datapath_error` code (no exceptions cross the boundary), and callbacks take a `void* user` plus an optional destructor for it. Option structs carry their size, so call `datapath_options_init()` before changing fields; passing `NULL` uses the defaults.

```c
static void echo(void* user, datapath_socket* s, const uint8_t* data, size_t size) {
    datapath_socket_send(s, data, size);
}
static int on_accept(void* user, datapath_server* server, datapath_socket* client) {
    datapath_socket_set_on_message(client, echo, NULL, NULL);
    keep(client);                                  // yours now; datapath_socket_release() it later
    return 1;                                      // 0 rejects (the library releases it)
}

datapath_server* server;
datapath_host("my-service", NULL, NULL, &server);
datapath_server_set_on_accept(server, on_accept, NULL, NULL);

datapath_socket* sock;
if (datapath_connect("my-service", NULL, NULL, &sock) == DATAPATH_SUCCESS)
    datapath_socket_send(sock, "hello", 5);
```
Built with `DATAPATH_SHARED=ON`, the library exports only the C API on Linux. The header documents threading and ownership rules in full.

## Getting DataPath
Add the repository as a subdirectory and link the `datapath::datapath` target:
```cmake
add_subdirectory(datapath)
target_link_libraries(my-app PRIVATE datapath::datapath)
```
Requires CMake 3.16+ and a C++17 compiler (MSVC 2019+, GCC 9+, Clang 10+). To build the library, tests and benchmarks on their own, see [CONTRIBUTING.md](CONTRIBUTING.md).

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
Wheels cover CPython 3.10+ (including free-threaded builds) on Windows and Linux. See [python/README.md](python/README.md) for the API, the two ways to receive and their latency.

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
