# DataPath vs libuv

libuv 1.52.1 over named pipes (Windows) and Unix domain sockets (Linux), through
`libuv-benchmark`: the same harness, framing, clock and sizes as
`datapath-benchmark`, with libuv's own event loop doing the I/O. Methodology and
machines are in the [comparisons overview](README.md).

* **DataPath shm**: defaults (`spin` 50 µs, `priority` 2, one I/O thread) with `options::shared_memory`.
* **DataPath socket / pipe**: the same defaults over the default transport.
* **libuv as shipped**: `--spin 0 --no-mmcss --priority 0`, libuv's loop as it comes.
* **libuv + policy**: libuv's loop thread given DataPath's spin and priority settings.

## Summary
* **The edge over libuv on the same transport is the scheduling policy.** Idle on Linux, libuv + policy (13.7 µs p99.9 at 16 B) lands next to DataPath's socket path (13.3 µs); libuv as shipped sleeps in `epoll_wait` after every message and pays the wake-up (23 µs). On Windows the policy brings libuv from 31 to 15 µs at 64 B, while DataPath's pipe sits at 6.9 µs.
* **Shared memory is the structural step.** No system call or kernel buffer on the data path: 1.2–1.8 µs p99.9 up to 1 KiB on Linux at worst-case placement, 8–20× below any socket variant, and 2.1–2.6 µs from 64 B to 1 KiB on Windows.
* **Under full load only shared memory stays in microseconds.** At worst-case placement on Linux every socket-based variant, DataPath's included, spends one round trip in a thousand waiting for a scheduler timeslice (3–4 ms p99.9 from 16 B), while DataPath over shared memory keeps 10–13 µs up to 1 KiB and 23 µs at 4 KiB. Its spinning receiver never leaves the CPU, so there is nothing to wake.
* **Large messages under load are hard for everyone.** From 64 KiB the copy takes long enough to be preempted, and every variant measures in milliseconds; shared memory is still the lowest, at about half the socket variants at 256 KiB and 1 MiB.

## Linux: merenka-gpu0, `--pin 1,5`
Client on CPU 1, server on CPU 5: different cores on different CCDs. 2026-11-23, DataPath `cdc663a`.

### Idle
p99.9 (µs), with p99.9 lo–hi of the runs for DataPath shm:

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | **1.18** | **1.43** | **1.43** | **1.83** | **3.44** | **8.93** | **22.0** | **76.6** | **321** |
| DataPath socket | 13.3 | 13.1 | 13.6 | 14.4 | 19.4 | 30.3 | 59.8 | 124 | 479 |
| libuv as shipped | 23.1 | 22.7 | 24.0 | 24.4 | 28.3 | 37.4 | 63.5 | 147 | 557 |
| libuv + policy | 13.7 | 13.7 | 14.3 | 15.2 | 19.6 | 26.2 | 56.3 | 136 | 541 |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 1.04 | 1.15 | 1.24 | 1.59 | 2.67 | 5.85 | 16.3 | 57.5 | 232 |
| DataPath socket | 9.91 | 9.96 | 10.4 | 11.3 | 15.5 | 25.1 | 52.5 | 110 | 437 |
| libuv as shipped | 18.4 | 18.7 | 19.2 | 20.1 | 23.2 | 31.4 | 56.6 | 133 | 529 |
| libuv + policy | 10.3 | 10.3 | 11.0 | 12.0 | 15.2 | 21.7 | 49.3 | 123 | 497 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 0.94 | 0.77 | 0.77 | 1.11 | 2.18 | 5.12 | 14.7 | 44.9 | 184 |
| DataPath socket | 9.41 | 9.49 | 9.71 | 10.4 | 14.8 | 24.1 | 48.9 | 105 | 413 |
| libuv as shipped | 17.7 | 18.1 | 18.4 | 19.1 | 22.2 | 28.9 | 52.6 | 124 | 516 |
| libuv + policy | 9.76 | 9.78 | 10.4 | 11.2 | 14.6 | 20.8 | 45.4 | 117 | 469 |

### `--stress 0 --stress-nice 12`
DataPath shm: median of three invocations (range of the three medians in brackets). Other rows: one invocation.

p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | **10.2** (10.2–10.6) | **10.1** (10.1–10.1) | **10.8** (10.4–10.9) | **13.2** (12.7–13.3) | **22.8** (21.4–36.3) | **50.8** (40.6–179) | **4,058** (3,726–4,088) | **4,207** (4,206–4,260) | **6,396** (5,382–6,480) |
| DataPath socket | 3,112 | 249 | 292 | 3,689 | 5,507 | 5,648 | 5,105 | 7,302 | 13,481 |
| libuv as shipped | 4,074 | 4,070 | 4,070 | 4,065 | 4,076 | 4,094 | 4,181 | 7,416 | 12,102 |
| libuv + policy | 3,382 | 3,494 | 3,553 | 3,885 | 4,924 | 4,755 | 4,643 | 7,558 | 12,966 |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 6.54 | 6.73 | 6.93 | 8.41 | 12.3 | 24.6 | 74.2 | 3,533 | 4,279 |
| DataPath socket | 94.6 | 84.4 | 89.1 | 144 | 248 | 2,305 | 3,928 | 4,525 | 10,459 |
| libuv as shipped | 234 | 236 | 240 | 267 | 318 | 357 | 3,169 | 4,506 | 9,500 |
| libuv + policy | 99.2 | 114 | 140 | 150 | 251 | 2,008 | 3,979 | 4,696 | 9,457 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 3.19 | 3.23 | 3.48 | 4.65 | 8.44 | 16.9 | 49.8 | 291 | 867 |
| DataPath socket | 29.6 | 30.1 | 32.6 | 36.8 | 71.2 | 151 | 267 | 560 | 2,088 |
| libuv as shipped | 127 | 129 | 133 | 152 | 196 | 225 | 311 | 728 | 2,778 |
| libuv + policy | 30.0 | 30.6 | 36.7 | 40.5 | 68.0 | 138 | 294 | 726 | 2,723 |

### Same-CCD reference (DataPath only)
`--pin 1,2`: client and server on different cores that share an L3. Not the published case; it shows what placement alone is worth (see [memory bandwidth](memory-bandwidth.md)).

| p99.9 (µs) | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm, idle | 0.73 | 0.74 | 0.77 | 0.86 | 1.19 | 2.26 | 7.82 | 19.1 | 65.7 |
| DataPath socket, idle | 9.26 | 9.34 | 9.72 | 10.6 | 15.1 | 30.6 | 54.0 | 82.3 | 239 |
| DataPath shm, stress | 3.91 | 3.83 | 4.06 | 5.22 | 9.31 | 22.6 | 56.7 | 3,967 | 6,408 |
| DataPath socket, stress | 33.8 | 35.0 | 35.1 | 39.3 | 87.1 | 161 | 3,113 | 6,106 | 7,632 |

## Windows: Irkela, `--pin 8,12`
Client on logical CPU 8, server on 12: two different physical cores, neither of them core 0. The 7940HS has one CCD and one NUMA node, so this is the worst placement the machine has. 2026-11-23, DataPath `df30376`; DataPath shm under stress is the median of three invocations.

p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm, idle | **8.2** | **2.2** | **2.1** | **2.6** | **4.1** | **8.6** | **18.9** | **42.3** | **130** |
| DataPath pipe, idle | 11.3 | 6.9 | 7.2 | 7.2 | 16.5 | 24.4 | 50.3 | 76.7 | 754 |
| libuv as shipped, idle | 31.0 | 30.7 | 29.8 | 30.6 | 34.5 | 44.7 | 65.8 | 278 | 1,075 |
| libuv + policy, idle | 15.7 | 15.2 | 15.1 | 15.2 | 22.2 | 34.6 | 73.5 | 279 | 9,659 |
| DataPath shm, stress | **5.1** | **5.8** | **7.2** | **15.3** | **25.6** | **74.3** | **528** | 3,946 | **24,394** |
| DataPath pipe, stress | 41.9 | 40.3 | 40.8 | 42.0 | 113 | 224 | 621 | **2,096** | 28,135 |
| libuv as shipped, stress | 244 | 265 | 282 | 293 | 401 | 1,076 | 1,600 | 36,406 | 90,842 |
| libuv + policy, stress | 105 | 104 | 114 | 111 | 187 | 273 | 653 | 7,469 | 31,434 |

DataPath p50 / p99.9 idle (µs); p50 for libuv was not recorded in this run:

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| shm p50 | 1.3 | 0.6 | 0.7 | 0.8 | 1.2 | 2.4 | 6.1 | 20.4 | 71.6 |
| pipe p50 | 3.5 | 3.2 | 3.2 | 3.1 | 7.2 | 11.3 | 26.3 | 50.8 | 538 |

Notes from the Windows runs:
* Without pinning, under stress each invocation depended on where the scheduler put the two I/O threads, for both libraries: 16 KiB p99.9 was about 15 µs with both on the two hyper-threads of one core, about 200 µs on separate cores, and about 1.1 ms when one was on core 0. That is why the published runs are pinned to separate cores.
* Under stress the pipe has the lower tail at 256 KiB (2.1 vs 2.3–4.0 ms over three repeats) and more throughput (735 vs 585 MiB/s); 64 KiB was stable at 446–529 µs over shared memory, still below the pipe's 621.
* At 64 KiB idle, shared memory moves 19 GiB/s against 6.3 GiB/s for libuv + policy.
* libuv + policy at 1 MiB idle measured 5–16 ms p99.9 in this run (1.2 ms in an earlier one). With 2,048 round trips per run, p99.9 at 1 MiB is the second or third slowest round trip, and one hiccup moves it.

## Why
* **libuv as shipped sleeps between messages.** Its loop blocks in `epoll_wait` / `GetQueuedCompletionStatus` as soon as it runs out of work, so every message pays a kernel wake-up and a context switch on both ends. DataPath's I/O threads busy-poll for `spin` (50 µs) after the last event; in a ping-pong the next message nearly always arrives inside that window.
* **Priority decides who waits under load.** With every CPU busy, a woken thread waits for a timeslice unless it outranks the load. DataPath's `priority` (thread priority on Windows, nice on Linux) is that gap; libuv has none. Giving libuv's thread the same policy recovers most of the socket-path difference, which is why "libuv + policy" and DataPath's socket path are close on Linux.
* **A socket still has to wake the receiver.** Even with spin and priority, each message over a socket or pipe is a system call on the sender and a readiness event on the receiver. Under full load at worst-case placement that is enough to lose a timeslice once in a thousand round trips. Shared memory removes it: the receiver polls a ring in memory it already has mapped.
* **Large messages are copies.** At 256 KiB and 1 MiB the time is spent copying, and where the two processes run decides how fast that is. See [memory bandwidth and placement](memory-bandwidth.md).
