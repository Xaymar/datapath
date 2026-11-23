# Memory bandwidth and placement

Is DataPath's large-message latency at the memory-bandwidth limit, or is it
spent signalling through the kernel and scheduler? Measured on merenka-gpu0
(AMD EPYC 7313P, container with CPUs 0–7 and 16–23 on two CCDs) on 2026-11-23,
DataPath `cdc663a` over shared memory, idle. A 1 MiB round trip copies 2 MiB:
the client copies its message into the ring, and the server copies the received
bytes into its reply.

**Short answer: it's copy time, and where the two processes run decides how fast
the copy is.** Signalling is about 13 µs of a 190 µs round trip. Pinned to
different CCDs, 1 MiB p99.9 is 329 µs; on one CCD it is 102 µs, and on the two
hyper-threads of one core 70 µs.

## memcpy on this machine
`membw.cpp`, one thread on CPU 1, per-copy time:

| Copy | p50 | p99.9 | GB/s at p50 |
|---|---|---|---|
| 64 KiB, in cache | 1.15 µs | 1.75 µs | 57 |
| 64 KiB, cold (512 MiB buffer set) | 4.04 µs | 9.13 µs | 16 |
| 256 KiB, in cache | 5.23 µs | 10.1 µs | 50 |
| 256 KiB, cold | 15.5 µs | 22.3 µs | 17 |
| 1 MiB, in cache | 25.2 µs | 33.7 µs | 42 |
| 1 MiB, cold | 61.1 µs | 81.8 µs | 17 |

Aggregate (1 MiB cold copies, all threads at once): 17.6 GB/s with one thread,
about 19–20 GB/s with 2–4 threads on one CCD, 33 GB/s with 8 threads across both
CCDs, 32 GB/s with all 16. One CCD tops out near 20 GB/s of copying.

## Where a 1 MiB round trip goes
The library and benchmark were instrumented locally with `CLOCK_MONOTONIC`
stamps around the ring copy, the eventfd write, and the receive (the
instrumentation is not part of DataPath). Per-phase times in µs, 5 runs of
2,048 round trips:

| `--pin` | Placement | round trip p50 / p99.9 | client copy p50 / p99.9 | server copy p50 / p99.9 | signal + wake per direction, p50 |
|---|---|---|---|---|---|
| 1,5 | different CCDs | 190 / 329 | 115 / 234 | 60 / 178 | 1.7 + 4.7 |
| 1,2 | same CCD | 60 / 102 | 26 / 36 | 33 / 43 | receiver still spinning, 0.25 |
| 1,17 | one core's two hyper-threads | 52 / 70 | 25 / 38 | 25 / 37 | receiver still spinning, 0.27 |

* **Copies are 92% of the round trip** across CCDs (175 of 190 µs at p50), and the slowest 0.1% of round trips are slow copies too: in the unpinned run that landed on two CCDs, the slowest round trips averaged 197 µs client copy and 105 µs server copy out of 329.
* **The same copy is 2–5× slower across CCDs.** The client writes into ring lines the server just read on the other CCD, and the server reads lines that sit dirty in the client's L3. Every cache line crosses the Infinity Fabric. The server's own writes stay in its L3, because the client never reads the reply payload in this benchmark; that is the likely reason the server copy is the faster one across CCDs.
* **Signalling is small.** An eventfd write costs about 1.7–2 µs and the wake-up about 4.5–4.7 µs per direction, 12–13 µs per round trip. A 100 ms spin (`--spin 100000`, so neither side ever sleeps) takes the round trip from 188 to 176 µs p50 and 321 to 307 µs p99.9, no more.
* **Cross-CCD copies outlast the spin.** At 1 MiB across CCDs the copy (115 µs) is longer than the default 50 µs `spin`, so the waiting side goes to sleep on every round trip (100% of round trips in the pinned run) and pays the wake-up. On one CCD the copy is short enough that the receiver almost never sleeps.

At 256 KiB the pattern is the same: 153 µs p99.9 across CCDs against 21.7 µs on one CCD, with the client copy at 29.5 vs 5.7 µs p50.

## Under load
256 KiB with `--stress 0 --stress-nice 12`, unpinned, three invocations: the
median round trip is still mostly copy (about 30 + 30 µs of 64 µs), but the
p99.9 tail (0.2–4 ms) is scheduling. Delivery gaps of milliseconds appear when
a receiver that went to sleep waits for a timeslice, and an eventfd write can
take up to 220 µs when waking the receiver preempts the sender. With a 100 ms
spin the receivers never sleep, but the tail stays at about 4 ms: then the
spinning thread itself is preempted.

## What this means
* For messages up to a few KiB, placement costs microseconds (1.2 vs 0.7 µs p99.9 at 16 B); for 64 KiB and up it is the main factor, 3–5×.
* Memory bandwidth is not the limit. A 1 MiB round trip across CCDs moves 2 MiB in 190 µs, 11 GB/s, against 17 GB/s for a single cold `memcpy` and 33 GB/s for the machine. The cost is cache coherence between CCDs.
* On Linux, DataPath leaves placement to the scheduler (`ideal_core` and `pair_peer` exist only on Windows so far). Keeping both ends of a busy 1:1 connection inside one L3 domain is the lever for large messages; the published numbers use the worst case regardless.
