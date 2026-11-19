# DataPath
DataPath is a high-performance, low-latency inter-process communication (IPC) library written in C++17. It moves length-framed messages between processes on the same machine, from a few bytes to many megabytes, from one server to many clients, with any number of threads sending at once.

## Features
* One API on Windows (named pipes + I/O completion ports) and Linux (Unix domain sockets + epoll).
* Sub-100 µs round trips: no timers or sleeps anywhere on the data path.
* Zero-copy receive: handlers get a pointer into the receive buffer; several small messages arrive per system call.
* Thread-safe, non-blocking `send()` with bounded queues and explicit backpressure (`TooLarge`, `WouldBlock`).
* Idle connections cost no CPU. A short, tunable busy-poll before sleeping keeps tail latency low under traffic.
* Permissions per server (`User`, `Group`, `World`), a version handshake, and validation before `on_accept`.
* No dependencies outside the C++ standard library and the OS.

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

| Windows, desktop PC (5-run medians, MMCSS on) | p50 | p99 | p99.9 (range) |
|---|---|---|---|
| 64 B | 3.3 µs | 5.2 µs | 12.7 µs (10.7–15.5) |
| 4 KiB | 5.9 µs | 10.0 µs | 20.2 µs (19.3–40.8) |
| 64 KiB | 26.4 µs | 37.3 µs | 83 µs (72–88) |
| 1 MiB | 0.55 ms | 0.87 ms | 1.14 ms |
| 16 MiB | 8.5 ms | 9.2 ms | 9.5 ms |

Large messages move at about 3.6 GiB/s per direction on Windows. With a 1 MiB bulk stream running at 1.7 GiB/s on another connection (two I/O threads), small-message round trips stay at p99.9 28–58 µs. The original DataPath reached 11.9 µs round-trip p99.9 on Windows 10; this version reaches 11–16 µs (5-run medians) for small messages with a 3.3 µs median. Single runs vary a lot at this resolution, so compare with `--runs 5`.

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
* **Windows scheduling.** By default the I/O threads register with the Multimedia Class Scheduler (`context_options::mmcss`, "Pro Audio" task) and run at `THREAD_PRIORITY_HIGHEST` (`context_options::priority`, 0–3), which stays in effect if MMCSS registration fails. Over 5-run medians, MMCSS cut 64 KiB p99.9 from ~190 µs to ~83 µs, and with `spin = 0` it cut 4 KiB p99.9 from ~120 µs to ~79 µs. Normal priority doubled large-message tails. Small messages measured the same (11–16 µs p99.9) with every option. `yield` and `alertable` showed no gain and are off.
* **Buffers.** `options::receive_buffer` (256 KiB) holds incoming frames; reads adapt their size to the traffic, so a large buffer doesn't slow down small messages. `options::pipe_buffer` (1 MiB) is the Windows kernel pipe buffer.
* **Tuning.** `context_options::spin` (default 50 µs) is how long an I/O thread busy-polls after the last event before sleeping in the kernel. Higher values cut tail latency for bursty traffic and cost CPU only while traffic flows. `0` disables spinning. On Windows the I/O threads also opt out of EcoQoS power throttling.
* **Paths.** A bare name maps to `\\.\pipe\<name>` on Windows and to the abstract socket `datapath.<name>` on Linux. On Linux, a path starting with `/` is a filesystem socket instead, created with mode `0600`/`0660`/`0666` for `User`/`Group`/`World` and removed on close.

## Building
Requires CMake 3.16+ and a C++17 compiler (MSVC 2019+, GCC 9+, Clang 10+).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```
```bash
cmake --build build --config Release
```
```bash
ctest --test-dir build -C Release --output-on-failure
```

Options: `DATAPATH_SHARED` (shared library, default `OFF`), `DATAPATH_BUILD_TESTS` and `DATAPATH_BUILD_SAMPLES` (default `ON` for top-level builds). The `datapath::datapath` target can be used through `add_subdirectory`.

Cross-compiling for Windows from Linux with mingw-w64 (tests run through Wine if it's installed):
```bash
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake
```

## Benchmarking
`datapath-benchmark local` starts an echo server in a second process and runs the client against it:
```bash
./build/samples/datapath-benchmark local
```
On Windows, run `build\samples\Release\datapath-benchmark.exe local`. Useful variations:
* `--spin 0` measures without busy-polling.
* `--load` adds a 1 MiB bulk stream on a second connection and reports its throughput.
* `--clients 8` runs eight concurrent ping-pong connections.
* `--threads 2` uses two I/O threads per process.
* `--sizes 8,64,4096` picks the message sizes.

You can also run `server <name>` and `client <name>` separately, in two terminals.

Single runs are noisy at microsecond resolution. `--runs 5` repeats every size five times and reports the median, plus the lowest and highest p99.9 of any run. `samples\benchmark\matrix.cmd` (Windows) and `samples/benchmark/matrix.sh` (Linux) run every option combination that way and collect the output in `benchmark-results.txt`.

For stable numbers on Windows, use the High Performance power plan. Run from a foreground console and close CPU-heavy applications.

## Comparing with other IPC libraries
Configure with `-DDATAPATH_BUILD_COMPARE=ON` to also build `libuv-benchmark`; CMake downloads libuv 1.52.1. It runs the same harness over libuv named pipes / Unix sockets: same framing, clock, sizes, `--runs`, `--stress`. It also uses the same `--spin`, `--priority` and `--mmcss` policy on its loop thread, so both libraries can be compared with identical settings. libuv itself has none of these, so use `--spin 0 --no-mmcss --priority 0` to measure libuv as it ships. The matrix scripts include libuv runs when the executable exists.

First Linux results (4 vCPU VM, 3-run medians, round trip):

| | 8 B p50 | 8 B p99.9 | 4 KiB p50 | 64 KiB p50 |
|---|---|---|---|---|
| DataPath, defaults | 4.8 µs | 27 µs | 7.5 µs | 28 µs |
| libuv, same spin policy | 5.5 µs | 35 µs | 8.3 µs | 28 µs |
| DataPath, `--spin 0` | 25.8 µs | 127 µs | 28.3 µs | 46 µs |
| libuv as shipped (no spin) | 27.0 µs | 142 µs | 29.1 µs | 46 µs |

On Linux both sit on epoll and Unix sockets, so the difference comes from the defaults (spinning, scheduling) and per-message overhead. Large messages tie at about 4 GiB/s.

## License
GNU Affero General Public License v3.0 or later, see `LICENSE`.
