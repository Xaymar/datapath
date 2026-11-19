# DataPath low-latency rewrite — design

Date: 2026-11-19
Status: approved 2026-11-19

## Intent

DataPath is a general-purpose local IPC library. Users send messages of any
size (bytes to gigabytes) at any rate (a few per second to saturating), with
many clients connected to one server and many threads sending at once.

The library predates Windows 10 2004/11 and was never modernized. Its wait
loops are millisecond-granular (`wait(0)` then `wait(1ms)` / `sleep_for(1ms)`),
which on modern Windows turns into 1–16 ms stalls whenever the poll misses, and
it allocates kernel events and heap objects per message. The original
implementation measured **11.9 µs full round trip at the 99.9th percentile**
(QueryPerformanceCounter, ping-pong, Windows 10) — about 100× faster than the
libuv-based messaging it was compared against. That number shows named pipes are fast enough; the
regressions come from DataPath's own waiting and allocation, not the transport.

## Goals

- **Full round trip p99.9 under 100 µs** on Windows 10/11 and Linux. Stretch:
  return to the original ~12 µs round-trip p99.9 on Windows with spinning
  enabled.
- **Throughput under load:** sustain at least the predecessor's
  (lib-streamlabs-datalane, overlapped I/O) ~1 GiB/s with round-trip latency
  staying under 10 ms while saturated. That was the ceiling on an AMD
  Bulldozer system; modern hardware should exceed it.
- Windows first, Linux second, in this project.
- Small to large messages up to a configurable maximum; tiny messages are not
  penalized by large-message support. Larger payloads are the application's
  job to split (user decision).
- Many-to-many: one server, many clients; `send()` safe from any number of
  threads on one connection; messages are atomic on the wire (never interleaved).
- Zero CPU use on idle connections by default.
- A benchmark that reports median, p99, p99.9 and max, measured the way the
  original was (QPC on Windows, `CLOCK_MONOTONIC` on Linux), so the user can
  compare like for like.
- Fix existing correctness bugs (dangling iterator capture in the server accept
  path, erase-during-iteration, overlapped `cancel()` closing an event `reset()`
  reuses, no peer-death handling).

## Non-goals

- Shared-memory transport. Ruled out: it does not fit the many-senders /
  many-receivers model DataPath is meant for.
- macOS, iOS, Android backends (the platform layer must leave room for them:
  kqueue / epoll-on-Android later).
- Network / remote IPC, encryption, authentication beyond OS permissions.
- Source compatibility with the current API.

## Anti-goals (failure even if it "works")

- Burning a core while idle. Spinning is bounded and opt-in-tunable.
- Any timer- or sleep-based wait on the data path.
- Latency that depends on `timeBeginPeriod` or global timer resolution.
- Per-message kernel object creation or per-message heap allocation for small
  messages in steady state.

## Constraints

- C++17 minimum (MSVC 2019+, GCC 9+, Clang 10+); no third-party dependencies.
- CMake build, AGPL-3.0 license headers kept.
- Must compile cleanly under MSVC with `NOMINMAX`/`WIN32_LEAN_AND_MEAN` issues
  handled inside the library (the Tardsplaya integration hit C2589 and missing
  `<limits>`).
- Development container is Linux: Linux is built, tested and benchmarked here;
  Windows is cross-compiled here with mingw-w64 and **benchmarked by the user
  on real Windows hardware**. Wine is used only for correctness smoke tests.

## Approaches considered

1. **Kernel transport done right (chosen).** Named pipes + I/O completion ports
   on Windows, Unix domain sockets + epoll on Linux, length-framed messages,
   pooled buffers, a small fixed set of I/O threads, optional spin-before-block.
   Meets the target, handles every size and rate, no idle CPU, and matches
   many-to-many naturally because the kernel arbitrates connections.
2. **Shared-memory rings with a pipe for setup.** 1–5 µs when spinning, but
   needs chunking for huge messages, complex crash/security handling, and does
   not fit many senders / many receivers. Rejected by the user.
3. **Fix the current loops in place** (shorter waits, `timeBeginPeriod(1)`).
   Rejected: still timer-bound, still one thread per socket, still allocating
   per message; cannot reliably hit the target on Windows 11 with power
   throttling.

## Design

### Public API (new, breaking)

```cpp
namespace datapath {
  struct options {
    std::chrono::microseconds spin{50};   // busy-poll budget before blocking; 0 = never spin
    size_t   io_threads = 1;              // completion/epoll threads per context
    uint64_t max_message_size = 64 << 20; // send() rejects larger with TooLarge; peer closes on violation
    uint64_t max_queued_bytes = 64 << 20; // send() returns WouldBlock when the queue is this full
    datapath::permissions permissions = permissions::User;
    size_t   backlog = 8;                 // pending accept instances (Windows) / listen backlog
    size_t   max_clients = 0;             // 0 = unlimited
  };

  class socket {
   public:
    // Called on an I/O thread. The span is valid only during the call.
    void on_message(std::function<void(const uint8_t* data, size_t size)>);
    void on_close(std::function<void(datapath::error)>);
    datapath::error send(const void* data, size_t size);          // thread-safe, copies, queues; never blocks
    datapath::error send(const iovec_like* parts, size_t count);  // gather: header+payload pieces
    datapath::error close();
    bool good() const;
  };

  class server {
   public:
    // Return false to reject. Called on an I/O thread.
    void on_accept(std::function<bool(std::shared_ptr<socket>)>);
    datapath::error close();
  };

  datapath::error connect(std::shared_ptr<socket>&, const std::string& path, const options& = {});
  datapath::error host(std::shared_ptr<server>&, const std::string& path, const options& = {});
}
```

- Handlers are set once (single callback), not a `std::list<std::function>`.
  Setting a handler before data is delivered is guaranteed by delivering only
  after `on_accept` returns (server) or after `connect` returns and the first
  handler is set (client: reads are armed when `on_message` is first set).
- `itask`, `waitable`, `threadpool`, `event`, `overlapped-queue` are removed.
  Completion of an individual send is not reported; `send` returns `Closed` /
  `Failure` synchronously, and failures after queuing surface via `on_close`.
- Backpressure (user decision: return errors, never block): `send()` returns
  `error::TooLarge` if the message exceeds `max_message_size` (the smaller of
  the two peers' limits, exchanged in the handshake) and `error::WouldBlock`
  if accepting it would push the queue past `max_queued_bytes`. A message is
  either fully queued or rejected, never partially.
- Ordering: messages from one thread on one socket arrive in order. Messages
  from different threads are each atomic; their relative order is the order
  they acquired the send queue.
- `on_message` for one socket is never called concurrently with itself.

### Wire format

8-byte little-endian header: `uint64_t length`. Then `length` payload bytes.
No flags or version field in v1 of the framing — a handshake message at
connect time carries a protocol version (magic `"DPTH"` + `uint32_t version`),
so the frame header stays minimal.

### Receive path

- Each connection owns a receive buffer (default 64 KiB, grows for large
  frames). One read can deliver many small frames; the parser walks every
  complete frame in the buffer and calls `on_message` with a pointer into it
  (zero copy).
- A frame larger than the buffer gets a dedicated allocation of exactly its
  size; the read for the remainder goes directly into it.
- Steady state for small messages: zero allocations, one read syscall per batch.

### Send path

- Per-connection send queue guarded by a mutex (lock held only to enqueue).
  Small messages (< 64 KiB) are copied with their header into pooled buffers;
  consecutive small messages are coalesced into one write.
- Large messages (up to `max_message_size`): header and payload copied into one
  owned buffer and written as a unit. One write is outstanding per connection
  at a time.
- Fast path: if the queue is empty and no write is in flight, the calling
  thread issues the write itself (no hand-off to the I/O thread).

### Windows backend

- Named pipes, `PIPE_TYPE_BYTE`, `FILE_FLAG_OVERLAPPED`,
  `PIPE_REJECT_REMOTE_CLIENTS`, buffer sizes from options.
- One I/O completion port per context; pipe handles associated with it.
  `SetFileCompletionNotificationModes(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS |
  FILE_SKIP_SET_EVENT_ON_HANDLE)` so synchronously-completed reads/writes are
  handled inline without a completion packet.
- I/O thread loop: `GetQueuedCompletionStatusEx(timeout 0)` repeatedly for up
  to `spin`, then `INFINITE`. No timers anywhere.
- I/O threads opt out of power throttling / EcoQoS
  (`SetThreadInformation(ThreadPowerThrottling)`), so Windows 11 doesn't park
  them on efficiency cores when the host app is in the background.
- Server keeps `backlog` instances with outstanding overlapped `ConnectNamedPipe`
  on the IOCP; a completion creates the socket, calls `on_accept`, and replaces
  the instance. `ERROR_PIPE_CONNECTED` handled inline.
- Client `connect` uses `CreateFileW` with `WaitNamedPipeW` retry on
  `ERROR_PIPE_BUSY` (bounded).
- Permissions map to SDDL security descriptors: `User` = current user SID
  only, `Group` = current user + BUILTIN\Users, `World` = Everyone. Built with
  `ConvertStringSecurityDescriptorToSecurityDescriptorW`.
- Peer death: read/write failures with `ERROR_BROKEN_PIPE` /
  `ERROR_NO_DATA` / `ERROR_PIPE_NOT_CONNECTED` close the socket and fire
  `on_close`.

### Linux backend

- `AF_UNIX`, `SOCK_STREAM`, non-blocking, `SOCK_CLOEXEC`. Paths: absolute
  filesystem paths used as-is; bare names go into the abstract namespace
  (`\0datapath.<name>`), mirroring `\\.\pipe\<name>` on Windows.
- One `epoll` per I/O thread, edge-triggered, `EPOLLIN|EPOLLOUT|EPOLLRDHUP`.
  Spin: `epoll_wait(…, 0)` for up to `spin`, then `epoll_wait(…, -1)`.
- Writes go straight from the calling thread when possible; `EAGAIN` hands the
  remainder to the I/O thread via `EPOLLOUT`.
- Permissions: filesystem socket mode bits (`0600`/`0660`/`0666`); abstract
  sockets checked with `SO_PEERCRED` against the policy.
- `MSG_NOSIGNAL` on sends; `SIGPIPE` never raised.

### Code layout

```
include/datapath/   datapath.hpp, socket.hpp, server.hpp, options.hpp, error.hpp, permissions.hpp
source/common/      framing (parser/encoder), buffer pool, send queue, spin-wait helper
source/windows/     iocp context, pipe socket, pipe server, security descriptor
source/linux/       epoll context, uds socket, uds server
tests/              correctness tests (ctest; no external framework)
samples/benchmark/  cross-process ping-pong: server and client modes
```

### Benchmark

- `datapath-benchmark server <name>` / `client <name> --size N --count N
  --spin us`. Client sends, server echoes, client timestamps with QPC
  (Windows) / `clock_gettime(CLOCK_MONOTONIC)` (Linux).
- Reports min, median, p99, p99.9, max of full RTT (the headline, matching the
  original 11.9 µs measurement), plus throughput.
- Size sweep default: 8 B, 64 B, 4 KiB, 64 KiB, 1 MiB, 16 MiB.
- Mode for N concurrent clients to exercise many-to-many.
- Load mode: saturate with bulk traffic on one or more connections while a
  separate ping-pong stream measures latency; reports GiB/s and the latency
  percentiles under load (target: ≥ 1 GiB/s, p99.9 RTT < 10 ms).

### Tests

Framing round-trip (including split reads at every byte offset), zero-length
messages, messages larger than the receive buffer, concurrent senders on one
socket (no interleaving), many clients on one server, peer killed mid-message,
server close with clients attached, `TooLarge` and `WouldBlock` returns,
peer violating the negotiated limit is disconnected, rejected accept.

### Runtime context (builder's call, marked)

A `datapath::context` owns the I/O threads and the IOCP/epoll instances.
`spin` and `io_threads` are context settings, not per-connection ones. A
process-wide default context is created lazily on first `connect`/`host`;
users who want separate tuning create their own `context` and pass it in
(`connect(sock, path, opts, ctx)`). Per-connection options are only
`max_message_size`, `permissions`, `backlog`, `max_clients`. All sockets on a
context share its threads, so 50 clients do not mean 50 spinning threads.
Destroying a context closes its sockets/servers and joins its threads.

### Handshake (builder's call, marked)

The client sends `"DPTH" + uint32 version + uint64 max_message_size` immediately after the pipe opens;
the server validates it **before** calling `on_accept` and replies with the
same structure. `connect()` blocks until the reply arrives (bounded, default
1 s → `TimedOut`). A mismatch makes `connect` return `error::VersionMismatch`
(new code) and the server drops the connection without calling `on_accept`.

### Permissions (builder's call, marked)

`permissions` becomes a single level (`User`, `Group`, `World`); the bitmask
operators are removed. Windows: `User` = owner SID, `Group` = owner +
BUILTIN\Users, `World` = Everyone. Linux: filesystem sockets use `0600` /
`0660` (creator's primary group) / `0666`; abstract sockets enforce the same
policy with `SO_PEERCRED` (uid equal / gid equal / anyone).

### Callback re-entrancy and lifetime (builder's call, marked)

- `send()` and `close()` may be called from inside `on_message`, `on_close`
  and `on_accept`, including on the same socket. The inline write fast path
  must not re-enter `on_message`.
- The library holds an internal reference to each socket while I/O is
  outstanding or a callback runs, so a callback never sees a destroyed socket.
- When the user drops the last `shared_ptr` to a socket, the connection is
  closed (RAII); on the server side, the user keeps sockets alive by keeping
  the `shared_ptr` handed to `on_accept`. Returning `true` but discarding the
  pointer closes the connection.
- `on_close` fires exactly once, on an I/O thread, after which no further
  callbacks run for that socket.

### Latency target scope (builder's call, marked)

The p99.9 < 100 µs target applies to messages up to 64 KiB, with the default
`spin`, at up to 8 concurrent clients. Larger sizes are reported for
throughput. `spin = 0` results are reported but not held to the target.

### Validation

mingw-w64 is available in the dev container: Windows code is cross-compiled
and the tests are smoke-run under Wine here. Real latency numbers come from
the user's Windows machine.

## Left to the builder

- Exact buffer-pool sizing and coalescing thresholds (tune by benchmark).
- Whether `io_threads > 1` shards connections round-robin or by load.
- Internal class names and file splits within the layout above.
- Default `spin` value — start at 50 µs, adjust with benchmark data.
