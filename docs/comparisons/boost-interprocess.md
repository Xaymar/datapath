# DataPath vs Boost.Interprocess

[Boost.Interprocess](https://www.boost.org/doc/libs/release/doc/html/interprocess.html)
provides shared memory, interprocess mutexes and condition variables, and on top
of them `message_queue`: a bounded queue of fixed-size slots in shared memory,
guarded by one mutex and two condition variables. `message_queue` is Boost's
ready-made way to pass messages between processes, so that is what was measured,
with `boost-benchmark`: the same harness, clock and sizes as `datapath-benchmark`,
on the same machines and on the same day as the DataPath rows. Methodology and
machines are in the [comparisons overview](README.md).

* Boost 1.92.0, header-only, release build.
* **Boost as shipped**: `--spin 0 --no-mmcss --priority 0`. The receive thread blocks in `receive()` (`timed_receive()` with a 100 ms timeout, to notice shutdown).
* **Boost + spin**: the receive thread first polls `try_receive()` for 50 µs (DataPath's `spin`) and runs at DataPath's thread priority, as with libuv.
* **Windows, two builds.** On Windows Boost.Interprocess defaults to its *generic emulation*: its mutex and condition variable are spin locks that wait with `pause`, then yield and sleep in a loop, and never wait on a kernel object. `boost-native-benchmark` defines `BOOST_INTERPROCESS_FORCE_NATIVE_EMULATION`, which switches to Windows kernel primitives. Shared memory is a memory-mapped file in `C:\ProgramData\boost_interprocess` in both builds (see [below](#how-boostinterprocess-differs)).
* How the transport uses it: Boost has no server or connection, so the server receives on a control queue where each client sends a hello with a unique id. The client creates two queues per connection, sized for the run's largest message (16 MiB per direction, at least 4 slots), and the server opens them. One receive thread per connection. Sends use `try_send()`, so a full queue reports `WouldBlock`.
* DataPath rows are `datapath-benchmark` with and without `options::shared_memory`.

Linux: merenka-gpu0, `--pin 1,5`, 2026-11-25, DataPath `a6a8085`. Windows: Irkela, `--pin 8,12`, Balanced plan, 2026-11-25, DataPath `31766bc` (same library code).

## Summary
* **Linux, idle:** `message_queue` costs 24–32 µs p99.9 up to 1 KiB, 15–20× DataPath over shared memory and about twice DataPath's Unix socket. As shipped, every message wakes a thread sleeping on a futex (p50 10 µs); with DataPath's spin p50 drops to 4 µs but p99 stays at 13–23 µs. At 1 MiB it takes 1.15 ms against 0.33 ms: four copies per round trip instead of two, each made while holding the queue's lock.
* **Linux, full load:** 310–460 µs p99.9 up to 4 KiB as shipped, milliseconds from 16 KiB. With DataPath's spin it is worse (0.8–0.9 ms up to 256 B, 3–4 ms above), most likely because each poll takes the queue's mutex, so a poller preempted while holding it stalls the sender. DataPath over shared memory: 10–22 µs up to 4 KiB.
* **Windows, idle, default build:** the fastest small-message numbers in this comparison, 0.7–1.5 µs p99.9 up to 1 KiB (DataPath over shared memory 2.6–7.6 µs in the same session), because both processes spin on shared memory and never wait on a kernel object. Large messages show the cost of the sleep fallback: 1 MiB 6.3 ms p99.9, single round trips of 43–45 ms.
* **Windows, full load, default build:** it never finished its first size, 16 B, in 30 minutes, with or without DataPath's spin. A spin lock that yields and sleeps cannot make progress against 16 busy threads. DataPath over shared memory: 9–22 µs up to 1 KiB.
* **Windows, native primitives:** 40–95 µs p99.9 idle up to 1 KiB; under full load 9–13 ms as shipped and 122–145 µs with DataPath's spin (7–14× DataPath).
* **What Boost does that DataPath doesn't:** message priorities, any number of senders and receivers on one queue, named objects that outlive the processes, and the building blocks (managed segments, allocators, containers in shared memory) for a custom design. A hand-built ring on Boost's primitives could do better than `message_queue`; it would also be most of what DataPath is.

## How Boost.Interprocess differs
| | Boost.Interprocess `message_queue` | DataPath |
|---|---|---|
| Model | A named queue any process can open; many senders and receivers. No server, accept or connection. | Server and connections: `host()`, `connect()`, `on_accept`. |
| Message size | Fixed per queue at creation (`max_msg_size`), as is the slot count; memory is `slots × max_msg_size` up front. | Up to `options::max_message_size` (64 MiB by default), ring sized independently. |
| Copies | `send()` copies into the queue, `receive()` copies out into your buffer: four per round trip. | Two per round trip; the receiver reads in place. |
| Backpressure | `send()` blocks; `try_send()` / `timed_send()` fail. | `send()` returns `WouldBlock` or `TooLarge`; never blocks. |
| Receiving | `receive()` / `try_receive()` / `timed_receive()` on your own thread. | Callbacks on the context's I/O threads. |
| Peer gone | Not reported. | `on_close`. |
| Lifetime | POSIX semantics: the queue exists until `remove()` (or a reboot), whether or not anyone has it open. | The connection's memory goes away with the processes. |
| Windows backing | A memory-mapped file in `C:\ProgramData\boost_interprocess\<boot time>`, so the queue behaves like POSIX shared memory (it survives its last handle). Native `windows_shared_memory` exists but `message_queue` can't use it. | A pagefile-backed section with a random name, readable only by the two users. |

Notes from running it: the two Windows stress runs that were killed at the time limit left their queues behind, six files and 67 MB in `C:\ProgramData\boost_interprocess`. On Linux the queues are `/dev/shm` objects, removed by `remove()`.

## Linux: merenka-gpu0, `--pin 1,5`
### Idle
p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | **1.35** | **1.44** | **1.42** | **2.09** | **3.58** | **8.85** | **22.3** | **82.2** | **328** |
| DataPath socket | 13.3 | 13.3 | 13.6 | 14.4 | 19.5 | 30.5 | 61.8 | 132 | 492 |
| Boost as shipped | 31.8 | 32.2 | 31.4 | 31.8 | 40.9 | 57.9 | 128 | 403 | 1,155 |
| Boost + spin | 24.4 | 24.5 | 26.9 | 27.6 | 43.6 | 65.0 | 115 | 393 | 1,174 |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 1.04 | 1.13 | 1.23 | 1.69 | 2.65 | 5.64 | 16.3 | 57.3 | 230 |
| DataPath socket | 9.66 | 10.0 | 10.1 | 11.1 | 15.3 | 25.3 | 52.4 | 113 | 438 |
| Boost as shipped | 27.2 | 27.2 | 27.2 | 27.5 | 34.3 | 49.2 | 119 | 382 | 895 |
| Boost + spin | 13.2 | 13.2 | 20.4 | 23.4 | 38.7 | 60.3 | 91.2 | 375 | 771 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 0.86 | 0.75 | 0.77 | 1.14 | 2.17 | 4.86 | 12.3 | 44.9 | 184 |
| DataPath socket | 9.05 | 9.35 | 9.30 | 10.2 | 14.3 | 24.0 | 48.6 | 104 | 412 |
| Boost as shipped | 9.99 | 9.99 | 10.1 | 10.2 | 17.7 | 21.9 | 79.7 | 134 | 382 |
| Boost + spin | 3.69 | 3.77 | 4.48 | 3.84 | 19.0 | 33.0 | 43.6 | 134 | 380 |

### `--stress 0 --stress-nice 12`
DataPath shm: median of three invocations, range of the three medians in brackets. Other rows: one invocation.

p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | **10.8 (10.1–32.8)** | **9.77 (9.72–35.5)** | **10.9 (10.6–25.8)** | **12.2 (12.1–14.2)** | **21.7 (20.2–24.3)** | **49.0 (46.0–55.4)** | **4,051 (248–4,060)** | **4,222 (4,211–4,263)** | **6,606 (4,517–6,726)** |
| DataPath socket | 3,694 | 904 | 502 | 3,338 | 4,379 | 5,596 | 5,758 | 7,396 | 13,407 |
| Boost as shipped | 385 | 464 | 369 | 370 | 313 | 3,008 | 4,688 | 10,829 | 18,061 |
| Boost + spin | 844 | 866 | 920 | 3,205 | 4,123 | 4,173 | 4,735 | 9,361 | 18,984 |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 6.83 | 6.51 | 6.95 | 8.31 | 12.9 | 25.4 | 72.8 | 3,780 | 4,262 |
| DataPath socket | 74.5 | 69.1 | 72.7 | 97.5 | 205 | 456 | 3,877 | 4,589 | 10,126 |
| Boost as shipped | 231 | 165 | 241 | 215 | 146 | 192 | 1,571 | 7,476 | 11,809 |
| Boost + spin | 455 | 480 | 505 | 529 | 612 | 770 | 2,399 | 7,118 | 13,795 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 3.06 | 3.21 | 3.49 | 4.71 | 8.55 | 17.3 | 49.3 | 297 | 847 |
| DataPath socket | 29.8 | 30.1 | 30.9 | 34.6 | 57.7 | 139 | 259 | 567 | 2,062 |
| Boost as shipped | 43.0 | 48.0 | 48.5 | 52.3 | 63.5 | 95.9 | 194 | 474 | 1,433 |
| Boost + spin | 9.84 | 10.4 | 12.3 | 18.0 | 52.8 | 141 | 228 | 466 | 1,451 |

## Windows: Irkela, `--pin 8,12`
DataPath over shared memory measured 7.1–7.6 µs p99.9 at 16 B and 64 B in this session, against 2.2 µs at 64 B on 2026-11-23 with the same library code; see the [cpp-ipc page](cpp-ipc.md#windows-irkela---pin-812).

### Idle
p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | **7.60** | **7.10** | **5.00** | **2.60** | **3.40** | **12.7** | **36.1** | **55.7** | **133** |
| DataPath pipe | 13.0 | 10.0 | 18.1 | 13.6 | 21.2 | 36.0 | 52.3 | 159 | 1,762 |
| Boost as shipped (spin emulation) | 0.70 | 1.10 | 1.50 | 1.10 | 4.10 | 28.9 | 54.5 | 208 | 6,310 |
| Boost + spin (spin emulation) | 0.90 | 1.00 | 1.20 | 1.30 | 8.70 | 26.6 | 46.4 | 222 | 672 |
| Boost native sync | 94.8 | 95.7 | 83.4 | 58.2 | 74.5 | 102 | 74.7 | 237 | 634 |
| Boost native sync + spin | 40.5 | 42.7 | 37.3 | 44.4 | 26.2 | 44.1 | 68.9 | 251 | 535 |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 6.00 | 6.00 | 1.20 | 1.30 | 1.50 | 3.00 | 8.20 | 24.1 | 103 |
| DataPath pipe | 8.90 | 4.10 | 4.40 | 4.40 | 6.30 | 13.7 | 32.8 | 62.7 | 769 |
| Boost as shipped (spin emulation) | 0.60 | 0.70 | 0.70 | 0.70 | 1.70 | 6.80 | 12.6 | 62.1 | 253 |
| Boost + spin (spin emulation) | 0.60 | 0.60 | 0.70 | 0.80 | 1.80 | 6.70 | 14.7 | 69.3 | 251 |
| Boost native sync | 19.5 | 20.0 | 19.6 | 19.7 | 20.8 | 25.2 | 29.9 | 74.3 | 283 |
| Boost native sync + spin | 17.0 | 17.2 | 17.1 | 17.3 | 17.7 | 21.6 | 25.8 | 66.5 | 255 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 1.10 | 1.10 | 0.70 | 0.80 | 1.20 | 2.40 | 6.10 | 20.4 | 71.2 |
| DataPath pipe | 3.30 | 3.00 | 3.10 | 3.00 | 4.90 | 11.4 | 26.3 | 52.6 | 542 |
| Boost as shipped (spin emulation) | 0.40 | 0.40 | 0.50 | 0.50 | 1.40 | 5.10 | 9.50 | 41.2 | 171 |
| Boost + spin (spin emulation) | 0.40 | 0.40 | 0.50 | 0.50 | 1.40 | 5.00 | 10.2 | 48.2 | 177 |
| Boost native sync | 16.1 | 16.2 | 16.2 | 16.4 | 17.3 | 20.6 | 24.2 | 58.7 | 223 |
| Boost native sync + spin | 9.30 | 11.6 | 11.7 | 11.8 | 10.7 | 16.5 | 19.9 | 55.0 | 214 |

### `--stress 0`
DataPath shm: median of three invocations, range of the three medians in brackets. Other rows: one invocation. Boost with its default spin emulation, with and without DataPath's spin, did not finish its first size, 16 B, in 30 minutes, so it has no rows.

p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | **8.90 (7.50–13.0)** | **10.2 (9.20–10.7)** | **15.7 (12.6–15.8)** | **21.7 (18.0–22.3)** | **43.8 (38.3–43.9)** | **165 (154–178)** | **2,331 (1,473–2,485)** | **4,005 (3,770–5,416)** | **20,810 (16,224–22,744)** |
| DataPath pipe | 54.8 | 60.1 | 66.2 | 61.7 | 161 | 364 | 1,155 | 2,265 | 28,550 |
| Boost native sync | 8,971 | 10,106 | 11,577 | 12,666 | 22,608 | 32,242 | 40,617 | 77,349 | 103,959 |
| Boost native sync + spin | 122 | 131 | 136 | 145 | 182 | 356 | 2,562 | 7,760 | 26,422 |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 4.10 | 4.50 | 5.60 | 9.00 | 24.3 | 76.8 | 337 | 1,736 | 7,012 |
| DataPath pipe | 32.1 | 33.2 | 34.8 | 34.8 | 86.4 | 231 | 472 | 1,339 | 13,969 |
| Boost native sync | 83.5 | 83.3 | 84.8 | 85.7 | 110 | 230 | 1,336 | 32,163 | 63,237 |
| Boost native sync + spin | 85.0 | 85.6 | 85.7 | 86.9 | 106 | 159 | 392 | 2,240 | 7,810 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 2.40 | 2.60 | 3.60 | 6.20 | 17.2 | 58.4 | 265 | 1,003 | 5,145 |
| DataPath pipe | 22.8 | 24.0 | 24.0 | 20.4 | 62.9 | 145 | 348 | 985 | 10,877 |
| Boost native sync | 49.8 | 49.6 | 52.1 | 53.0 | 70.1 | 120 | 316 | 1,140 | 6,248 |
| Boost native sync + spin | 33.1 | 33.3 | 33.6 | 31.2 | 51.0 | 93.9 | 297 | 1,231 | 6,451 |
