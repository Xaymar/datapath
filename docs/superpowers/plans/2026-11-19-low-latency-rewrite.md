# DataPath Low-Latency Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace DataPath's timer/poll-based Windows implementation with a completion-driven named-pipe (IOCP) backend and a Unix-socket (epoll) backend that achieve full-round-trip p99.9 < 100 µs.

**Architecture:** A platform-neutral core (framing, handshake, bounded send queue, spin-then-block helper, public API handles) sits over a per-platform `detail::context_impl` that owns I/O threads and creates `detail::connection` / `detail::listener` objects. Public `socket`/`server` are RAII handles over internal shared objects.

**Tech Stack:** C++17, CMake ≥ 3.16, Win32 (named pipes, IOCP, SDDL), Linux (AF_UNIX, epoll, eventfd), mingw-w64 + Wine for Windows cross-build/smoke tests, ctest.

**Spec:** `docs/superpowers/specs/2026-11-19-low-latency-rewrite-design.md`

## Global Constraints

- C++17 minimum; no third-party dependencies.
- AGPL-3.0 headers on every source file.
- No timer- or sleep-based waits on the data path. The only bounded waits are the connect handshake (`connect_timeout`, default 1000 ms) and shutdown joins.
- Defaults: `spin = 50 µs`, `io_threads = 1`, `max_message_size = 64 MiB`, `max_queued_bytes = 64 MiB`, `backlog = 8`, `max_clients = 0` (unlimited), `receive_buffer = 64 KiB`.
- Frame header: 8 bytes, little-endian `uint64_t` length. Handshake: `"DPTH"`, `uint32_t version = 1`, `uint64_t max_message_size` (16 bytes, little-endian).
- `send()` never blocks: `TooLarge` above the negotiated limit, `WouldBlock` when the queue would exceed `max_queued_bytes` (an empty queue always accepts a frame within the limit).
- Windows headers are included only from `source/windows/*` with `NOMINMAX` and `WIN32_LEAN_AND_MEAN` defined; no public header includes `<windows.h>`.

## Review Focus

- A peer that disconnects mid-frame: the other side fires `on_close(Closed)` once, never delivers a partial message. → `integration: peer_dies_mid_frame` (raw client writes half a frame then closes).
- `close()` or dropping the last handle from inside `on_message`: no crash, no further callbacks, `on_close` once. → `integration: close_inside_callback`, `drop_inside_callback`.
- Server `close()` while clients are mid-transfer: all clients get `on_close`, `host()` on the same name succeeds afterwards. → `integration: server_close_then_rehost`.
- Garbage / wrong-version handshake from an untrusted client: never reaches `on_accept`, server keeps serving others. → `integration: bad_handshake_rejected`.
- Frame header above the receiver's limit sent by a raw peer: connection closed, no allocation of the claimed size. → `integration: oversized_header_disconnects`.

---

### Task 1: Build system, public API surface, test harness, legacy removal

**Files:**
- Delete: `include/*` (old), `source/threadpool.cpp`, `source/windows/*` (old), `samples/single-process-ipc/*`, `samples/benchmark/*`.
- Create: `include/datapath/{datapath,error,permissions,options,context,socket,server}.hpp`, `source/common/api.cpp`, `source/common/detail.hpp`, `cmake/toolchains/mingw-w64.cmake`, `tests/check.hpp`, `tests/CMakeLists.txt`, `tests/test_api.cpp`.
- Modify: `CMakeLists.txt` (rewrite; keep project name/version + `version.hpp` generation; `cmake/clang` included only if present).

**Interfaces (Produces):**
- `enum class datapath::error : int32_t { Unknown=-1, Success, Failure, CriticalFailure, Closed, TimedOut, InvalidPath, NotSupported, TooLarge, WouldBlock, VersionMismatch, NotFound, AddressInUse, AccessDenied }; const char* to_string(error)`.
- `enum class datapath::permissions : uint8_t { User, Group, World }`.
- `struct datapath::context_options { std::chrono::microseconds spin{50}; size_t io_threads = 1; }`.
- `struct datapath::options { uint64_t max_message_size; uint64_t max_queued_bytes; permissions permissions; size_t backlog; size_t max_clients; std::chrono::milliseconds connect_timeout{1000}; size_t receive_buffer; }`.
- `struct datapath::buffer_view { const void* data; size_t size; }`.
- `class datapath::context { static std::shared_ptr<context> create(const context_options& = {}); static std::shared_ptr<context> get_default(); }`.
- `class datapath::socket` (non-copyable; destructor closes): `on_message(std::function<void(const uint8_t*, size_t)>)` (first call arms reads), `on_close(std::function<void(error)>)`, `error send(const void*, size_t)`, `error send(const buffer_view*, size_t)`, `error close()`, `bool good() const`, `uint64_t max_message_size() const`.
- `class datapath::server` (destructor closes): `on_accept(std::function<bool(std::shared_ptr<socket>)>)`, `error close()`.
- `error datapath::connect(std::shared_ptr<socket>&, const std::string& path, const options& = {}, std::shared_ptr<context> = nullptr)`; `error datapath::host(std::shared_ptr<server>&, const std::string& path, const options& = {}, std::shared_ptr<context> = nullptr)`.
- `detail.hpp`: abstract `detail::connection` (mirrors socket methods), `detail::listener` (`set_accept_handler`, `close`), `detail::context_impl` (`connect(...)`, `listen(...)`), `std::shared_ptr<detail::context_impl> detail::make_platform_context(const context_options&)`.
- `tests/check.hpp`: `TEST(name)` registration + `CHECK(expr)` / `CHECK_EQ(a,b)`; `int main` runs all or the one named in `argv[1]`; each `TEST` registered with ctest via `datapath_add_test(file)`.

- [ ] Step 1: Install Wine (`apt-get install -y wine64`) and confirm a mingw hello-world `.exe` runs under it. If Wine fails, Windows ctest is disabled and Windows is compile-only.
- [ ] Step 2: Write `tests/test_api.cpp`: `error_to_string` (every enumerator maps to a non-empty distinct string), `options_defaults` (checks every default listed in Global Constraints).
- [ ] Step 3: Implement headers, `api.cpp`, stub `make_platform_context` per platform returning a context whose `connect`/`listen` return `NotSupported`; CMake with `DATAPATH_BUILD_TESTS`, `DATAPATH_BUILD_SAMPLES`, `DATAPATH_SHARED`; mingw toolchain with static libgcc/libstdc++ and `CMAKE_CROSSCOMPILING_EMULATOR=wine`.
- [ ] Step 4: Run Linux and mingw builds + ctest; both PASS.
- [ ] Step 5: Commit.

### Task 2: Framing, handshake, spin helper

**Files:** Create `source/common/{framing,handshake,spin}.hpp` (+ `.cpp` where non-template), `tests/test_framing.cpp`.

**Interfaces (Produces):**
- `constexpr size_t frame_header_size = 8; void encode_frame_header(uint8_t* out, uint64_t length);`
- `class frame_parser { frame_parser(uint64_t max_message_size, size_t buffer_size); std::pair<uint8_t*, size_t> prepare(); template<class F> error commit(size_t bytes_read, F&& deliver); void set_limit(uint64_t); }` — `prepare()` returns where the next read goes; `commit` delivers every complete frame (pointer into the buffer, zero copy) and returns `TooLarge` for a header over the limit **before** allocating. Frames larger than the buffer get a dedicated exact-size buffer filled by subsequent reads.
- `constexpr uint32_t protocol_version = 1; constexpr size_t hello_size = 16; struct hello { uint32_t version; uint64_t max_message_size; }; void encode_hello(uint8_t* out, const hello&); error decode_hello(const uint8_t* in, hello& out);` (`VersionMismatch` on bad magic or version).
- `template<class Poll, class Block> void spin_then_block(std::chrono::microseconds budget, Poll&& poll, Block&& block)` — call `poll()` (returns `true` when it handled work, which restarts the budget) until the budget elapses with no work, then `block()` once.

- [ ] Step 1: Tests: `parser_every_split_offset` (3 frames of sizes 0, 5, 70000 concatenated, fed in every 2-way split + byte-by-byte; delivered sequence identical), `parser_many_small_per_read`, `parser_rejects_oversize_header` (limit 1024, header 1025 → `TooLarge`, no delivery), `hello_roundtrip`, `hello_bad_magic`, `hello_bad_version`, `spin_zero_budget_blocks_immediately`, `spin_work_defers_block`.
- [ ] Step 2: Run → FAIL (missing symbols). Step 3: implement. Step 4: PASS on Linux + Wine. Step 5: Commit.

### Task 3: Bounded coalescing send queue

**Files:** Create `source/common/send_queue.{hpp,cpp}`, `tests/test_send_queue.cpp`.

**Interfaces (Produces):**
- `class send_queue { send_queue(uint64_t max_message_size, uint64_t max_queued_bytes); error push(const buffer_view* parts, size_t count, bool& acquired_writer); bool take(std::vector<uint8_t>& batch); void recycle(std::vector<uint8_t>&& buf); void set_limit(uint64_t); void close(); }`.
- `push` frames the parts (header + payload) atomically under a mutex; small frames append to the tail block while it stays ≤ 64 KiB, larger frames get their own block; returns `Closed` after `close()`. `acquired_writer` becomes `true` for exactly one caller when nobody holds the write slot. `take` (writer only) swaps the front block into `batch` and returns `true`, or releases the write slot and returns `false` when empty — atomically, so no message is stranded.

- [ ] Step 1: Tests: `push_rejects_too_large`, `would_block_when_full` (limit 1 MiB: 64 KiB frames accepted until next would exceed → `WouldBlock`; after `take` accepted again), `empty_queue_accepts_frame_above_queue_limit`, `coalesces_small_frames` (100 × 10 B → one `take`), `single_writer_slot`, `concurrent_push_no_interleave` (8 threads × 10 000 frames; one writer drains; parse output with `frame_parser`; every frame intact, per-thread order preserved).
- [ ] Steps 2–5: fail, implement, pass (Linux + Wine), commit.

### Task 4: Windows backend + shared integration suite

**Files:** Create `source/windows/{win.hpp,path.{hpp,cpp},iocp_context.{hpp,cpp},pipe_connection.{hpp,cpp},pipe_server.{hpp,cpp}}`, `tests/test_integration.cpp`, `tests/raw_peer.{hpp,cpp}` (platform raw connect/write/close helpers).

**Decisions:**
- Paths: bare name → `\\.\pipe\<name>` (backslashes → `/`), names already starting with `\\.\pipe\` kept; too long → `InvalidPath`.
- `iocp_context`: one IOCP, `io_threads` threads running `spin_then_block` over `GetQueuedCompletionStatusEx(…, 64, …, 0 / INFINITE)`. Every operation is an `io_op : OVERLAPPED` holding `std::shared_ptr<owner>` keepalive (the internal reference) released on completion. Handles get `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE`; synchronous success is processed inline in a loop (never recursion). Shutdown: close all handles, wait for outstanding-op count 0, post one null-overlapped quit packet per thread, join.
- Connection: one read op in flight once armed; one write op in flight (send-queue writer slot). A send whose caller acquires the writer slot issues `WriteFile` from the caller thread.
- Server: `backlog` instances (first with `FILE_FLAG_FIRST_PIPE_INSTANCE`; failure → `AddressInUse`) each with overlapped `ConnectNamedPipe`; `PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS`, buffers = `receive_buffer`. On connect: replace the instance immediately (respecting `max_clients`), read hello, validate, call `on_accept`, and only if it returns `true` send the hello reply and hand the socket over. Rejected/invalid → close without reply.
- Client: `CreateFileW` (`FILE_FLAG_OVERLAPPED`); `ERROR_FILE_NOT_FOUND` → `NotFound`, `ERROR_ACCESS_DENIED` → `AccessDenied`, `ERROR_PIPE_BUSY` → `WaitNamedPipeW` until `connect_timeout`. Handshake uses an event-based OVERLAPPED **before** IOCP association; no reply within `connect_timeout` → `TimedOut`, pipe closed by server → `Closed`. Negotiated limit = min of both.
- Errors `ERROR_BROKEN_PIPE`, `ERROR_NO_DATA`, `ERROR_PIPE_NOT_CONNECTED`, `ERROR_OPERATION_ABORTED` → `on_close(Closed)`; anything else → `on_close(Failure)`. `on_close` fires exactly once.
- Security attributes: default (null DACL descriptor) in this task; Task 5 adds real permissions.

- [ ] Step 1: Integration tests (all platform-neutral, unique names from pid + counter): `connect_missing_is_not_found`, `host_twice_is_address_in_use`, `echo_small`, `zero_length`, `large_8mib_intact`, `ordered_10000`, `concurrent_senders` (8 threads × 1000 mixed-size messages, verify intact + per-thread order), `many_clients` (16 × 100 echoes), `too_large_negotiated` (server limit 1 KiB → client `send(2048)` = `TooLarge`), `would_block_when_peer_stalls` (server never arms reads; client 1 MiB sends with `max_queued_bytes` 4 MiB hit `WouldBlock` before 1 GiB total), `rejected_accept_connect_fails` (`Closed`), `drop_handle_closes_peer`, `close_inside_callback`, `drop_inside_callback`, `on_close_exactly_once`, `server_close_then_rehost`, plus the five Review Focus tests.
- [ ] Step 2: Run under Wine → FAIL (`NotSupported`). Step 3: implement. Step 4: all PASS under Wine (repeat 3× for flakiness). Step 5: Commit.

### Task 5: Windows permissions + power-throttling opt-out

**Files:** Create `source/windows/security.{hpp,cpp}`; modify `pipe_server.cpp`, `iocp_context.cpp`.

- SDDL from `GetTokenInformation(TokenUser)` SID string: `User` = `D:P(A;;GA;;;<sid>)(A;;GA;;;SY)`, `Group` = User + `(A;;GRGW;;;BU)`, `World` = User + `(A;;GRGW;;;WD)`; `ConvertStringSecurityDescriptorToSecurityDescriptorW`; descriptor freed with `LocalFree` on server close.
- I/O threads call `SetThreadInformation(ThreadPowerThrottling, {version 1, ControlMask = EXECUTION_SPEED, StateMask = 0})`, resolved with `GetProcAddress` (absent before Win10 1709: ignored).
- [ ] Test `security_descriptor_builds_for_each_level` (descriptor valid via `IsValidSecurityDescriptor`); integration suite still passes under Wine; commit.

### Task 6: Linux backend

**Files:** Create `source/linux/{path.{hpp,cpp},epoll_context.{hpp,cpp},uds_connection.{hpp,cpp},uds_server.{hpp,cpp}}`.

**Decisions:**
- Paths: leading `/` → filesystem socket; otherwise abstract `"\0datapath.<name>"`; over `sizeof(sun_path)` → `InvalidPath`.
- `epoll_context`: `io_threads` threads, each with its own epoll + eventfd (quit/wake); connections assigned round-robin; `spin_then_block` over `epoll_wait(…, 0 / -1)`; edge-triggered `EPOLLOUT|EPOLLRDHUP` at creation, `EPOLLIN` added (EPOLL_CTL_MOD) when reads are armed.
- Writes: `sendmsg`/`send` with `MSG_NOSIGNAL` from the writer-slot holder; on `EAGAIN` the remaining batch is parked and resumed by the I/O thread on `EPOLLOUT`.
- Server: filesystem path in use → probe-connect; `ECONNREFUSED` → unlink and bind, success → `AddressInUse`. `chmod` 0600/0660/0666 by permissions. Abstract sockets: `SO_PEERCRED` check (uid equal / gid equal / any). `accept4(SOCK_NONBLOCK|SOCK_CLOEXEC)` until `EAGAIN`. Handshake read async, then `on_accept`, reply only on accept. `listen` backlog = `backlog` (min 8).
- Client connect: blocking handshake with `poll()` bounded by `connect_timeout`, then non-blocking + epoll. `ENOENT`/`ECONNREFUSED` → `NotFound`, `EACCES` → `AccessDenied`.
- Peer death: `read` → 0, `ECONNRESET`, `EPIPE` → `on_close(Closed)`.
- [ ] Step 1: Add `linux_permissions_mode_bits` test (filesystem socket mode per level). Step 2: run → FAIL. Step 3: implement. Step 4: whole suite PASS on Linux (repeat 10×, also under `-fsanitize=address,undefined` and a separate `-fsanitize=thread` build). Step 5: commit.

### Task 7: Benchmark + example

**Files:** Create `samples/benchmark/{main.cpp,stats.hpp,clock.hpp}`, `samples/echo/main.cpp`, `samples/CMakeLists.txt`.

- `datapath-benchmark server <name> [--spin us] [--threads n]` echoes every message.
- `datapath-benchmark client <name> [--sizes 8,64,4096,65536,1048576,16777216] [--count n] [--clients n] [--spin us] [--load]` — ping-pong per size, timestamps with QPC (Windows) / `CLOCK_MONOTONIC` (Linux); prints per size: `size count min p50 p99 p99.9 max` (µs, full RTT) and MiB/s. `--load`: one extra connection streams 1 MiB messages flat out while ping-pong runs; reports GiB/s and latency percentiles under load.
- `datapath-benchmark local` runs server + client in two processes (self re-exec) for one-command use.
- `samples/echo`: ≤ 80-line API example (host, connect, send, receive).
- [ ] Run Linux benchmark (spin 50 and spin 0, with and without `--load`); record results in the commit message and README. Smoke-run under Wine. Commit.

### Task 8: README and cleanup

- Rewrite `README.md`: features, platform table, API example, build (Linux, MSVC, mingw), benchmark instructions for Windows, design notes (spin/latency trade-off, backpressure errors), Linux measured numbers. Remove `.gitmodules` entry dependency (optional include). Commit and push.
