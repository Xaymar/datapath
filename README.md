# DataPath
**Fast, predictable message passing between processes on the same machine.**

DataPath is a C++17 IPC library for programs that talk to each other locally and care about the worst case, not just the average. It moves whole, length-framed messages, from a few bytes to many megabytes, between one server and any number of clients, with any number of threads sending at once. It uses each OS's native transport (named pipes with I/O completion ports on Windows, Unix domain sockets with epoll on Linux) behind one small API, and keeps round trips in the single-digit to low double-digit microseconds at the 99.9th percentile.

![Round-trip p99.9 latency: DataPath vs libuv](docs/images/latency-p999.svg)

On a Windows PC under full CPU load, DataPath's 8-byte round trip stays at 23 µs p99.9, while libuv as shipped stalls past 1 ms. On Linux, where both libraries sit on the same epoll and Unix-socket stack, DataPath's defaults keep the tail about 5× lower than libuv's, and stay ahead when libuv is given the same busy-poll policy. Details and the configs behind each bar are under [Comparison with libuv](#comparison-with-libuv).

## Features
**Latency**
* No timers or sleeps on the data path. An I/O thread busy-polls for a short, tunable time (`spin`, default 50 µs) after the last event, then sleeps in the kernel, so tail latency stays low under traffic and idle connections cost no CPU.
* Scheduling tuned for the tail: elevated I/O thread priority (thread priority on Windows, nice value on Linux where `RLIMIT_NICE` allows), opt-out of Windows EcoQoS throttling, and one I/O thread per physical core without sharing hyper-threads.
* Zero-copy receive: handlers get a pointer straight into the receive buffer, and several small messages arrive per system call.

**Messaging**
* Whole messages, never byte streams: each `send()` arrives as one `on_message` call, and messages from different threads never interleave.
* One server, many clients; `send()` is thread-safe and non-blocking from any thread, including from inside handlers.
* Bounded queues with explicit backpressure: `send()` returns `TooLarge` or `WouldBlock` instead of blocking or growing without limit.
* Large messages at 3.6–4 GiB/s per direction, without slowing down small messages on other connections.

**Safety and setup**
* Per-server access control (`User`, `Group`, `World`), a version handshake, and peer validation before `on_accept` runs.
* Clean shutdown: destroying a handle waits for running handlers, and no handler fires afterwards.
* One API on Windows and Linux, no dependencies outside the C++ standard library and the OS, usable as a CMake subdirectory.

## Platforms
| Platform | Transport | Status |
|---|---|---|
| Windows 10 / 11 | Named pipes, IOCP | Supported |
| Linux | Unix domain sockets, epoll | Supported |
| Android | Unix domain sockets, epoll | Planned (backend should apply) |
| macOS / iOS | Unix domain sockets, kqueue | Planned |

## Measured latency
Full round trip (send → echo → receive), ping-pong between two processes, release build, default settings (`spin` 50 µs, one I/O thread). Ranges span several runs on a shared cloud VM, where tail latency varies with neighbour load:

| Linux, 4 vCPU Xeon VM | p50 | p99 | p99.9 |
|---|---|---|---|
| 8 B | 4.7–5.1 µs | 8–11 µs | 28–48 µs |
| 4 KiB | 7.6 µs | 19 µs | 35–45 µs |
| 64 KiB | 37.5 µs | 60–68 µs | 95–190 µs |
| 8 B, `spin` 0 | 26 µs | 52 µs | 95 µs |

While another connection streams 1 MiB messages at 4–5 GiB/s, small-message round trips stay at p99.9 ≈ 2 ms with one I/O thread, and ≈ 0.25 ms with two.

| Windows 10 desktop PC, idle (5-run medians) | p50 | p99 | p99.9 (range) |
|---|---|---|---|
| 8 B | 3.0 µs | 4.5 µs | 7.5 µs (6.6–8.5) |
| 64 B | 3.0 µs | 4.5 µs | 7.3 µs (7.3–9.1) |
| 4 KiB | 6.4 µs | 10.0 µs | 18.6 µs (17.7–19.5) |
| 64 KiB | 19.9 µs | 33.8 µs | 75 µs (56–100) |
| 1 MiB | 0.54 ms | 0.92 ms | 1.07 ms |
| 16 MiB | 8.4 ms | 9.3 ms | 9.7 ms |

Under load (Prime95 plus `--stress 0` on every core) the defaults hold: p50 3.3 µs and p99.9 23 µs for 8 B, 60 µs for 4 KiB and 37 µs for 64 KiB.

Large messages move at about 3.6 GiB/s per direction on Windows. With a 1 MiB bulk stream running at 2.1 GiB/s on another connection (two I/O threads), small-message round trips stay at p99.9 16–27 µs. The original DataPath reached 11.9 µs round-trip p99.9 on Windows 10; this version reaches 7–8 µs for small messages with a 3.0 µs median (5-run medians). Single runs vary a lot at this resolution, so compare with `--runs 5`.

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
* **Core placement (Windows).** By default (`context_options::ideal_core = -1`) each I/O thread gets its own physical core as a soft hint (ideal processor, not hard affinity), never two on the hyper-threads of one core, and core 0, which handles most interrupts, is skipped when there are more than two cores. A busy hyper-thread sibling roughly halves the speed of the other one; on the test machine this cut 64 KiB p99.9 from 72 to 52 µs and 8 B from 12 to 7 µs. `-2` leaves placement to the OS, `N` starts at physical core N. `io_threads = 0` starts exactly one I/O thread per physical core (on Windows, the cores this placement uses), so no two I/O threads ever share a core's hyper-threads.
* **Buffers.** `options::receive_buffer` (256 KiB) holds incoming frames; reads adapt their size to the traffic, so a large buffer doesn't slow down small messages. `options::pipe_buffer` (1 MiB) is the Windows kernel pipe buffer.
* **Tuning.** `context_options::spin` (default 50 µs) is how long an I/O thread busy-polls after the last event before sleeping in the kernel. Higher values cut tail latency for bursty traffic and cost CPU only while traffic flows. `0` disables spinning. Spinning only helps while spare CPUs exist: when spinning I/O threads cover every CPU (for example client and server on one machine, both with `io_threads = 0`), a woken thread waits for a spinner's budget to run out, and on Linux 64 B p99.9 measured about 116 µs with the default spin versus 23 µs with `0`. `context_options::yield` also fixes that case (24 µs) but gives a timeslice away on every yield under full CPU load. On Windows the I/O threads also opt out of EcoQoS power throttling.
* **Paths.** A bare name maps to `\\.\pipe\<name>` on Windows and to the abstract socket `datapath.<name>` on Linux. On Linux, a path starting with `/` is a filesystem socket instead, created with mode `0600`/`0660`/`0666` for `User`/`Group`/`World` and removed on close.

## Getting DataPath
Add the repository as a subdirectory and link the `datapath::datapath` target:
```cmake
add_subdirectory(datapath)
target_link_libraries(my-app PRIVATE datapath::datapath)
```
Requires CMake 3.16+ and a C++17 compiler (MSVC 2019+, GCC 9+, Clang 10+). To build the library, tests and benchmarks on their own, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Comparison with libuv
The benchmark ships a second transport over libuv 1.52.1 named pipes / Unix sockets with the same framing, clock, message sizes and harness, so both libraries are measured the same way. "libuv as shipped" means libuv's own event loop with no busy-polling and normal thread priority; "same spin policy" gives libuv's loop thread DataPath's spin and priority settings. How to reproduce these runs is in [CONTRIBUTING.md](CONTRIBUTING.md#benchmarking).

First Linux results (4 vCPU VM, 3-run medians, round trip):

| | 8 B p50 | 8 B p99.9 | 4 KiB p50 | 64 KiB p50 |
|---|---|---|---|---|
| DataPath, defaults | 4.8 µs | 27 µs | 7.5 µs | 28 µs |
| libuv, same spin policy | 5.5 µs | 35 µs | 8.3 µs | 28 µs |
| DataPath, `--spin 0` | 25.8 µs | 127 µs | 28.3 µs | 46 µs |
| libuv as shipped (no spin) | 27.0 µs | 142 µs | 29.1 µs | 46 µs |

On Linux both sit on epoll and Unix sockets, so the difference comes from the defaults (spinning, scheduling) and per-message overhead. Large messages tie at about 4 GiB/s.

First Windows results, both under Prime95 load (DataPath additionally under `--stress 0`), p50 / p99.9:

| | 8 B | 64 B |
|---|---|---|
| DataPath, defaults | 3.3 / 23 µs | 3.4 / 27 µs |
| DataPath, `--spin 0` | 17.8 / 67 µs | 18.7 / 70 µs |
| libuv as shipped | 71.9 / 1081 µs | 74.8 / 1072 µs |

## Known issues (work in progress)
* No server-side handshake timeout: a local client that connects and never sends its hello holds a connection slot.

## License
GNU Affero General Public License v3.0 or later, see `LICENSE`.
