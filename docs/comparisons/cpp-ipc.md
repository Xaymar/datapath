# DataPath vs cpp-ipc

[cpp-ipc](https://github.com/mutouyun/cpp-ipc) (libipc) is a C++ IPC library for
Linux, Windows and FreeBSD built on shared memory: lock-free rings of 64-byte
slots, named channels that any process can open as sender or receiver, and a
blocking `recv()`. It is the closest library to DataPath's shared-memory
transport. It was measured with `cppipc-benchmark`: the same harness, clock and
sizes as `datapath-benchmark`, on the same machines and on the same day as the
DataPath rows. Methodology and machines are in the [comparisons overview](README.md).

* cpp-ipc `2e28547` (master, 2026-02-19). The last release, v1.4.1, lacks the later MSVC and MinGW fixes.
* **cpp-ipc as shipped**: `--spin 0 --no-mmcss --priority 0`. The receive thread blocks in `recv()`, which yields 32 times and then sleeps on a process-shared condition variable; every `send()` broadcasts that condition.
* **cpp-ipc + spin**: the same, but the receive thread first polls `try_recv()` for 50 µs (DataPath's `spin`) and runs at DataPath's thread priority, as with libuv.
* How the transport uses cpp-ipc: cpp-ipc has no server or connection, so the server receives on a control channel (`ipc::channel`) where each client sends a hello with a unique id. Each connection is then a pair of single-producer single-consumer unicast channels (`ipc::chan<single, single, unicast>`, the cheapest policy it has), with one receive thread per connection. Sends use `try_send()` with no timeout, so a full ring reports `WouldBlock` instead of overwriting.
* DataPath rows are `datapath-benchmark` with and without `options::shared_memory`.

Linux: merenka-gpu0, `--pin 1,5`, 2026-11-25, DataPath `a6a8085`. Windows: Irkela, `--pin 8,12`, Balanced plan, 2026-11-25, DataPath `31766bc` (same library code).

## Summary
* **Small messages, idle:** cpp-ipc takes 3.8–4.7 µs p99.9 up to 64 B on Linux, against 1.4 µs for DataPath over shared memory, and 7–8.6 µs from 256 B to 4 KiB against 1.4–3.6 µs. Messages above 64 B leave the ring for a chunk pool, which adds about 1.5 µs to every round trip. On Windows cpp-ipc is at 10–18 µs p99.9 up to 64 B.
* **Large messages, idle:** both copy each message twice per round trip, and from 64 KiB the copies dominate: on Linux cpp-ipc with DataPath's spin matches DataPath (1 MiB 314 vs 328 µs p99.9). As shipped, cpp-ipc's own waiting costs it at 256 KiB (169 vs 82 µs).
* **Under full load cpp-ipc as shipped stops working.** On Linux every size has p99 ≈ 4 ms and p99.9 ≈ 8 ms from 16 B up, and none of three invocations got past 4 KiB in 30 minutes. On Windows it did not complete its first size, 16 B, in 30 minutes. DataPath over shared memory stays at 10–12 µs up to 1 KiB on Linux and 9–22 µs on Windows.
* **DataPath's spin rescues cpp-ipc only for tiny messages.** With it, cpp-ipc holds 35–46 µs up to 1 KiB on Linux (3–4× DataPath), but from 64 KiB it is at ≈ 8 ms p99.9 and from 256 KiB even its p50 is 4.2 ms. On Windows everything above 64 B, where the chunk pool is used, takes 20–40 ms at p99.9, and 64 KiB did not finish in 30 minutes.
* **What cpp-ipc does that DataPath doesn't:** broadcast to up to 32 receivers per channel, and connection-less named channels any process can join. What it doesn't do: connections, accept, notice that a peer went away, or guarantee delivery (`send()` overwrites the oldest unread message after a timeout).

## Why
* **Waiting by yielding.** cpp-ipc's receive loop and its internal spin locks call `std::this_thread::yield()` (Linux `sched_yield`) before they block, and its spin locks fall back to `sleep_for(1 ms)`. On an idle machine a yield returns at once. Under full load it hands the CPU to a runnable stress thread, and the waiter comes back one scheduler timeslice later: the 4 ms and 8 ms steps in every Linux stress row, and the 20–40 ms rows on Windows, where a 1 ms sleep can last a whole 15.6 ms timer tick. We did not instrument cpp-ipc, so this is the likely cause rather than a measured one. DataPath spins with `pause` and then sleeps in the kernel until the peer signals it, so it never gives its time slice away while a message is due.
* **The chunk pool.** A message above 64 bytes is copied into one of 32 shared chunks for its size (rounded up to 1 KiB), guarded by a spin lock, and the receiver hands it back when it is done. That costs the extra 1.5 µs idle, and under load it is a lock that a preempted process can hold for a timeslice.
* **Per-message work.** Each `send()` broadcasts a process-shared condition variable (a lock and unlock even when nobody waits), each `recv()` does the same for the sender's condition, and messages up to 64 bytes are copied out of the ring into a buffer from cpp-ipc's allocator. DataPath's ring needs one atomic store per message and signals only a peer that is actually asleep.

## How cpp-ipc differs
| | cpp-ipc | DataPath |
|---|---|---|
| Model | Named channels; any process opens one as sender and/or receiver. No server, accept or connection. | Server and connections: `host()`, `connect()`, `on_accept`. |
| Delivery | Broadcast to every receiver of a channel, up to 32 receivers. | One-to-one per connection; any number of clients per server. |
| Message size | Up to 64 B inline in a 256-slot ring; larger through a pool of 32 chunks per size (rounded up to 1 KiB); if the pool is empty, split into 64-byte fragments. Under 2 GiB. | Up to `options::max_message_size` (64 MiB by default) in one ring record. |
| Backpressure | `send()` waits up to a timeout (100 ms by default), then overwrites the oldest unread message. `try_send()` fails instead. | `send()` returns `WouldBlock` or `TooLarge`; nothing is dropped. |
| Receiving | Blocking `recv()` / `try_recv()` on your own thread. | Callbacks on the context's I/O threads. |
| Peer gone | Not reported; a receiver keeps waiting. | `on_close`. |
| Cleanup | Shared memory lives in named objects (`/dev/shm` on Linux) until `clear_storage()`; a killed process leaves them behind. | Linux: an unnamed, sealed memfd passed over the socket. Windows: a section with a random name, readable only by the two users, gone with the last handle. Nothing outlives the processes. |
| Platforms | Linux, Windows, FreeBSD. | Linux, Windows, macOS (sockets). |

Notes from running it: cpp-ipc logs about 40 `fail shm_unlink` (`ENOENT`) errors when a process exits, because it removes some objects twice; the benchmark numbers are unaffected. Its shared objects outlive a killed process: the capped runs left 30 objects in `/dev/shm`.

## Linux: merenka-gpu0, `--pin 1,5`
### Idle
p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | **1.35** | **1.44** | **1.42** | **2.09** | **3.58** | **8.85** | **22.3** | **82.2** | **328** |
| DataPath socket | 13.3 | 13.3 | 13.6 | 14.4 | 19.5 | 30.5 | 61.8 | 132 | 492 |
| cpp-ipc as shipped | 4.74 | 4.74 | 6.98 | 7.16 | 8.22 | 12.5 | 28.7 | 169 | 341 |
| cpp-ipc + spin | 4.04 | 3.84 | 7.18 | 7.52 | 8.55 | 12.8 | 26.7 | 79.3 | 314 |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 1.04 | 1.13 | 1.23 | 1.69 | 2.65 | 5.64 | 16.3 | 57.3 | 230 |
| DataPath socket | 9.66 | 10.0 | 10.1 | 11.1 | 15.3 | 25.3 | 52.4 | 113 | 438 |
| cpp-ipc as shipped | 3.32 | 3.29 | 4.65 | 4.82 | 5.88 | 10.6 | 20.6 | 78.6 | 241 |
| cpp-ipc + spin | 3.13 | 3.11 | 4.31 | 4.44 | 5.34 | 10.2 | 20.5 | 68.8 | 259 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 0.86 | 0.75 | 0.77 | 1.14 | 2.17 | 4.86 | 12.3 | 44.9 | 184 |
| DataPath socket | 9.05 | 9.35 | 9.30 | 10.2 | 14.3 | 24.0 | 48.6 | 104 | 412 |
| cpp-ipc as shipped | 2.65 | 2.65 | 4.15 | 4.19 | 5.15 | 7.94 | 19.4 | 63.3 | 200 |
| cpp-ipc + spin | 2.70 | 2.68 | 4.04 | 4.18 | 5.02 | 7.60 | 19.0 | 64.2 | 249 |

### `--stress 0 --stress-nice 12`
DataPath shm and both cpp-ipc rows: median of three invocations, range of the three medians in brackets. DataPath socket: one invocation. cpp-ipc as shipped never got past 4 KiB: each of its three invocations hit the 30-minute limit there (n/a).

p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | **10.8 (10.1–32.8)** | **9.77 (9.72–35.5)** | **10.9 (10.6–25.8)** | **12.2 (12.1–14.2)** | **21.7 (20.2–24.3)** | **49.0 (46.0–55.4)** | **4,051 (248–4,060)** | **4,222 (4,211–4,263)** | **6,606 (4,517–6,726)** |
| DataPath socket | 3,694 | 904 | 502 | 3,338 | 4,379 | 5,596 | 5,758 | 7,396 | 13,407 |
| cpp-ipc as shipped | 7,921 (4,042–7,933) | 5,758 (4,220–7,905) | 7,995 (7,982–7,996) | 7,999 (7,996–8,000) | 8,015 (8,008–8,018) | n/a | n/a | n/a | n/a |
| cpp-ipc + spin | 36.2 (33.6–106) | 34.8 (32.5–36.2) | 46.3 (45.4–48.9) | 45.5 (41.9–54.0) | 222 (53.5–3,044) | 731 (94.1–4,096) | 7,923 (7,915–7,951) | 8,095 (8,049–8,100) | 8,091 (8,059–8,356) |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 6.83 | 6.51 | 6.95 | 8.31 | 12.9 | 25.4 | 72.8 | 3,780 | 4,262 |
| DataPath socket | 74.5 | 69.1 | 72.7 | 97.5 | 205 | 456 | 3,877 | 4,589 | 10,126 |
| cpp-ipc as shipped | 3,986 | 3,990 | 4,049 | 4,113 | 7,918 | n/a | n/a | n/a | n/a |
| cpp-ipc + spin | 21.7 | 22.2 | 28.2 | 25.3 | 33.1 | 64.7 | 4,055 | 8,033 | 8,038 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 3.06 | 3.21 | 3.49 | 4.71 | 8.55 | 17.3 | 49.3 | 297 | 847 |
| DataPath socket | 29.8 | 30.1 | 30.9 | 34.6 | 57.7 | 139 | 259 | 567 | 2,062 |
| cpp-ipc as shipped | 7.91 | 8.01 | 10.1 | 11.6 | 15.7 | n/a | n/a | n/a | n/a |
| cpp-ipc + spin | 8.22 | 8.64 | 10.8 | 11.7 | 15.8 | 31.0 | 77.7 | 4,203 | 4,535 |

## Windows: Irkela, `--pin 8,12`
DataPath over shared memory measured 7.1–7.6 µs p99.9 at 16 B and 64 B in this session (p99 6 µs), against 2.2 µs at 64 B on 2026-11-23 with the same library code; from 256 B up it matches the earlier runs. 16 B and 64 B are the first sizes of each run, so this looks like a warm-up effect of the Balanced power plan, but it has not been tracked down.

### Idle
p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | **7.60** | **7.10** | **5.00** | **2.60** | **3.40** | **12.7** | **36.1** | **55.7** | **133** |
| DataPath pipe | 13.0 | 10.0 | 18.1 | 13.6 | 21.2 | 36.0 | 52.3 | 159 | 1,762 |
| cpp-ipc as shipped | 16.8 | 18.0 | 28.2 | 27.5 | 30.6 | 42.5 | 152 | 104 | 569 |
| cpp-ipc + spin | 10.3 | 10.2 | 13.8 | 20.7 | 20.3 | 25.9 | 39.1 | 98.1 | 170 |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 6.00 | 6.00 | 1.20 | 1.30 | 1.50 | 3.00 | 8.20 | 24.1 | 103 |
| DataPath pipe | 8.90 | 4.10 | 4.40 | 4.40 | 6.30 | 13.7 | 32.8 | 62.7 | 769 |
| cpp-ipc as shipped | 3.60 | 4.10 | 7.30 | 8.70 | 22.8 | 29.7 | 41.1 | 82.4 | 177 |
| cpp-ipc + spin | 3.30 | 3.40 | 6.50 | 6.50 | 6.90 | 9.60 | 20.3 | 61.7 | 155 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 1.10 | 1.10 | 0.70 | 0.80 | 1.20 | 2.40 | 6.10 | 20.4 | 71.2 |
| DataPath pipe | 3.30 | 3.00 | 3.10 | 3.00 | 4.90 | 11.4 | 26.3 | 52.6 | 542 |
| cpp-ipc as shipped | 2.90 | 3.10 | 5.00 | 5.00 | 5.30 | 7.70 | 30.9 | 60.1 | 138 |
| cpp-ipc + spin | 2.80 | 2.90 | 4.30 | 4.40 | 4.60 | 6.60 | 13.9 | 43.7 | 54.2 |

### `--stress 0`
DataPath shm and cpp-ipc + spin: median of three invocations, range of the three medians in brackets. DataPath pipe: one invocation. cpp-ipc + spin never finished 64 KiB: each invocation hit the 30-minute limit there (n/a). cpp-ipc as shipped did not finish its first size, 16 B, in 30 minutes, so it has no row.

p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | **8.90 (7.50–13.0)** | **10.2 (9.20–10.7)** | **15.7 (12.6–15.8)** | **21.7 (18.0–22.3)** | **43.8 (38.3–43.9)** | **165 (154–178)** | **2,331 (1,473–2,485)** | **4,005 (3,770–5,416)** | **20,810 (16,224–22,744)** |
| DataPath pipe | 54.8 | 60.1 | 66.2 | 61.7 | 161 | 364 | 1,155 | 2,265 | 28,550 |
| cpp-ipc + spin | 44.3 (28.8–66.1) | 41.4 (27.8–68.8) | 22,464 (21,304–30,699) | 27,338 (18,290–32,425) | 33,166 (30,384–33,413) | 40,896 (39,874–41,436) | n/a | n/a | n/a |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 4.10 | 4.50 | 5.60 | 9.00 | 24.3 | 76.8 | 337 | 1,736 | 7,012 |
| DataPath pipe | 32.1 | 33.2 | 34.8 | 34.8 | 86.4 | 231 | 472 | 1,339 | 13,969 |
| cpp-ipc + spin | 10.5 | 10.9 | 38.4 | 40.5 | 55.3 | 17,264 | n/a | n/a | n/a |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 2.40 | 2.60 | 3.60 | 6.20 | 17.2 | 58.4 | 265 | 1,003 | 5,145 |
| DataPath pipe | 22.8 | 24.0 | 24.0 | 20.4 | 62.9 | 145 | 348 | 985 | 10,877 |
| cpp-ipc + spin | 8.00 | 8.10 | 25.1 | 26.4 | 32.4 | 54.3 | n/a | n/a | n/a |
