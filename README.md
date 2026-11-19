# DataPath
**Fast, predictable message passing between processes on the same machine.**

DataPath is a C++17 IPC library for programs that talk to each other locally and care about the worst case, not just the average. It moves whole, length-framed messages, from a few bytes to many megabytes, between one server and any number of clients, with any number of threads sending at once. It uses each OS's native transport (named pipes with I/O completion ports on Windows, Unix domain sockets with epoll on Linux and kqueue on macOS) behind one small API, and keeps small-message round trips at 7–13 µs p99.9 on an idle machine.

![Round-trip p99.9 latency: DataPath vs libuv](docs/images/latency-p999.svg)

Small messages are where DataPath is strongest. On Windows, its 8 B round trip has a p99.9 of 10 µs idle and 110 µs with every core busy, against 114 µs and 757 µs for libuv as shipped. Much of that comes from DataPath's defaults (a short busy-poll and a raised I/O thread priority): give libuv the same policy and it gets close on Linux, and at 64 KiB it is ahead on both platforms. The [benchmarks](#benchmarks) section has the full tables and what each configuration means.

## Features
**Latency**
* No timers or sleeps on the data path. An I/O thread busy-polls for a short, tunable time (`spin`, default 50 µs) after the last event, then sleeps in the kernel, so tail latency stays low under traffic and idle connections cost no CPU.
* Scheduling tuned for the tail: elevated I/O thread priority (thread priority on Windows, nice value on Linux where `RLIMIT_NICE` allows, quality-of-service class on macOS), opt-out of Windows EcoQoS throttling, and one I/O thread per physical core without sharing hyper-threads.
* Zero-copy receive: handlers get a pointer straight into the receive buffer, and several small messages arrive per system call.

**Messaging**
* Whole messages, never byte streams: each `send()` arrives as one `on_message` call, and messages from different threads never interleave.
* One server, many clients; `send()` is thread-safe and non-blocking from any thread, including from inside handlers.
* Bounded queues with explicit backpressure: `send()` returns `TooLarge` or `WouldBlock` instead of blocking or growing without limit.
* Large messages at about 4 GiB/s per direction on both platforms.

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
Full round trip (send, echo, receive), ping-pong between two processes, measured on 2026-11-19 at commit `1e4be58`, release builds, median of 5 runs. The benchmark ships a second transport over libuv 1.52.1 named pipes / Unix sockets with the same framing, clock, sizes and harness, so both libraries are measured the same way. How to reproduce these runs is in [CONTRIBUTING.md](CONTRIBUTING.md#benchmarking).

* **DataPath**: defaults (`spin` 50 µs, `priority` 2, one I/O thread).
* **libuv as shipped**: `--spin 0 --no-mmcss --priority 0`, libuv's own loop.
* **libuv + policy**: libuv's loop thread given DataPath's spin and priority settings.
* **Stress**: `--stress 0`, one busy thread per logical core in a separate process. On Linux the stress process runs at nice 12 (`--stress-nice 12`), which is the gap `priority` 2 gives when the system lets the process raise its priority. The test container had `RLIMIT_NICE` 0, so the gap was created from the other side; at equal nice every variant measured 4–8 ms, one scheduler timeslice.

**Windows: Irkela**, AMD Ryzen 9 7940HS (8 cores / 16 threads), Windows 10 22H2, Balanced power plan. p99.9 in µs:

| | 8 B | 64 B | 4 KiB | 64 KiB |
|---|---|---|---|---|
| DataPath, idle | **10.2** | **7.4** | **12.3** | 326 |
| libuv as shipped, idle | 114 | 306 | 305 | 513 |
| libuv + policy, idle | 16.6 | 15.9 | 22.1 | **101** |
| DataPath, stress | **110** | **58** | **1001** | 1453 |
| libuv as shipped, stress | 757 | 960 | 1038 | **1287** |
| libuv + policy, stress | 667 | 656 | 1071 | 1316 |

**Linux: merenka-gpu0**, AMD EPYC 7313P container limited to 8 cores / 16 threads, Linux 6.12, GCC 15.2. p99.9 in µs:

| | 8 B | 64 B | 4 KiB | 64 KiB |
|---|---|---|---|---|
| DataPath, idle | **13.1** | **13.0** | 20.0 | 85 |
| libuv as shipped, idle | 18.5 | 16.7 | 22.7 | **56** |
| libuv + policy, idle | 13.7 | 13.8 | **19.8** | 69 |
| DataPath, stress | 128 | **35** | 86 | **458** |
| libuv as shipped, stress | 152 | 115 | 203 | 4172 |
| libuv + policy, stress | **55** | 53 | **20** | 596 |

What the numbers say:
* On Windows, DataPath has the lowest tail up to 4 KiB in every case: 1.6–2× lower than libuv with the same policy when idle, and 6–11× lower at 8 B and 64 B under stress. At 4 KiB under stress all three sit near 1 ms.
* On Linux both libraries sit on the same epoll and Unix-socket stack. Idle, DataPath's lead over libuv as shipped comes from its spin policy, and libuv with the same policy ties it. Under stress the results are mixed and vary a lot between runs (single-run p99.9 ranged from 12 to 190 µs for DataPath at 64 B).
* At 64 KiB, libuv is ahead when idle on both platforms (Windows: 101 vs 326 µs and 5.9 vs 4.0 GiB/s). This is a known gap.
* Single runs are noisy at microsecond resolution; compare medians of several runs (`--runs 5`).

DataPath-only percentiles on the same machines, idle, in µs:

| | Irkela p50 | p99 | p99.9 | merenka-gpu0 p50 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| 8 B | 3.2 | 8.0 | 10.2 | 9.0 | 9.8 | 13.1 |
| 64 B | 3.0 | 4.1 | 7.4 | 9.0 | 9.7 | 13.0 |
| 4 KiB | 4.9 | 7.2 | 12.3 | 14.5 | 15.5 | 20.0 |
| 64 KiB | 33.1 | 47.4 | 326 | 42.5 | 61.8 | 85 |

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
* **Core placement (Windows).** By default (`context_options::ideal_core = -1`) each I/O thread gets its own physical core as a soft hint (ideal processor, not hard affinity), never two on the hyper-threads of one core, and core 0, which handles most interrupts, is skipped when there are more than two cores. A busy hyper-thread sibling roughly halves the speed of the other one; on the test machine this cut 64 KiB p99.9 from 72 to 52 µs and 8 B from 12 to 7 µs. `-2` leaves placement to the OS, `N` starts at physical core N. `io_threads = 0` starts exactly one I/O thread per physical core (on Windows, the cores this placement uses), so no two I/O threads ever share a core's hyper-threads.
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

## Known issues (work in progress)
* No server-side handshake timeout: a local client that connects and never sends its hello holds a connection slot.

## License
BSD 3-Clause License, see `LICENSE`.
