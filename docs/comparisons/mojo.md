# DataPath vs Chromium Mojo

[Mojo](https://chromium.googlesource.com/chromium/src/+/main/mojo/README.md) is
the IPC system of Chromium and everything built on it. It was measured with a
benchmark that builds inside a Chromium checkout and copies `samples/benchmark`'s
methodology and output format ([source and build steps](mojo/README.md)), on the
same machine and in the same session as DataPath. Methodology and machine are in
the [comparisons overview](README.md).

* Chromium `a57585d` (main, 2026-11-22), release build (`is_debug=false`, `dcheck_always_on=false`). Mojo core is ipcz-only in this version; on desktop Linux its links run over a Unix domain socket.
* **Mojo raw pipe**: a message pipe through the C API. A trap fires on Mojo's IO thread, and the handler reads the message without copying and writes the next one in place. No task posting: the cheapest way to use Mojo.
* **Mojo bindings, main thread**: `mojo::Remote` / `mojo::Receiver` of `Ping(array<uint8>) => (array<uint8>)`, bound on the main thread as browser code does. Every message hops from Mojo's IO thread to the main thread, on both sides.
* **Mojo bindings, IO thread**: the same interface bound on the IO thread (no hop).
* **Mojo BigBuffer, main thread**: `PingBig(mojo_base.mojom.BigBuffer)`, Chromium's type for large payloads, which puts anything above 64 KiB into a shared-memory region.
* DataPath rows are `datapath-benchmark` with and without `options::shared_memory`, `cdc663a`.

All runs on merenka-gpu0 at worst-case placement, `--pin 1,5` (each whole process on one CPU, different cores and CCDs).

## Summary
* **Small messages, idle:** Mojo's cheapest path costs 30–35 µs p99.9 and browser-style bindings 60–63 µs, against 1.2–1.8 µs for DataPath over shared memory and 13–14 µs over its socket. That is about 20–50× for shared memory, and 2.2–4.7× for DataPath on the same kind of transport.
* **Small messages, full load:** Mojo is at 4.1–4.2 ms p99.9 without the thread hop and 7.6–7.9 ms with it (up to 1 KiB), and main-thread bindings reach 5.6 ms already at p99. DataPath over shared memory stays at 10–13 µs: 400–800× lower.
* **Large messages:** inline Mojo messages run at about 150 MiB/s per direction (1 MiB round trip 6.8–8.0 ms p50 idle). BigBuffer fixes most of that (1.6 ms), and is still 5–8× above DataPath over shared memory from 256 KiB up idle, and about 2× under load.
* **What Mojo does that DataPath doesn't:** handle and shared-memory passing, a broker for sandboxed processes, a routing node network, typed and validated messages, interface versioning. Part of the cost below pays for that; part of it is the thread hop and per-message allocation, which a data path doesn't need.

## Idle

p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 1.18 | 1.43 | 1.43 | 1.83 | 3.44 | 8.93 | 22.0 | 76.6 | 321 |
| DataPath socket | 13.3 | 13.1 | 13.6 | 14.4 | 19.4 | 30.3 | 59.8 | 124 | 479 |
| Mojo raw pipe | 32.5 | 34.9 | 30.5 | 34.2 | 37.9 | 50.9 | 390 | 1,617 | 7,606 |
| Mojo bindings, IO thread | 34.8 | 33.8 | 34.9 | 35.7 | 45.9 | 58.9 | 506 | 2,027 | 8,476 |
| Mojo bindings, main thread | 60.5 | 61.7 | 62.5 | 63.1 | 72.8 | 86.6 | 439 | 2,003 | 8,704 |
| Mojo BigBuffer, main thread | 63.2 | 63.0 | 63.2 | 63.8 | 72.7 | 87.0 | 535 | 597 | 1,777 |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 1.04 | 1.15 | 1.24 | 1.59 | 2.67 | 5.85 | 16.3 | 57.5 | 232 |
| DataPath socket | 9.91 | 9.96 | 10.4 | 11.3 | 15.5 | 25.1 | 52.5 | 110 | 437 |
| Mojo raw pipe | 26.3 | 26.4 | 26.3 | 27.2 | 32.5 | 42.5 | 355 | 1,513 | 7,043 |
| Mojo bindings, IO thread | 29.4 | 29.3 | 29.3 | 29.4 | 38.3 | 50.3 | 429 | 1,773 | 8,158 |
| Mojo bindings, main thread | 51.4 | 51.2 | 51.7 | 51.9 | 62.9 | 73.2 | 400 | 1,805 | 8,078 |
| Mojo BigBuffer, main thread | 54.2 | 54.9 | 54.6 | 55.6 | 62.8 | 74.5 | 446 | 574 | 1,630 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm | 0.94 | 0.77 | 0.77 | 1.11 | 2.18 | 5.12 | 14.7 | 44.9 | 184 |
| DataPath socket | 9.41 | 9.49 | 9.71 | 10.4 | 14.8 | 24.1 | 48.9 | 105 | 413 |
| Mojo raw pipe | 21.9 | 22.1 | 22.4 | 22.9 | 28.5 | 37.2 | 337 | 1,461 | 6,796 |
| Mojo bindings, IO thread | 25.9 | 25.7 | 25.9 | 26.5 | 33.6 | 44.0 | 400 | 1,729 | 7,993 |
| Mojo bindings, main thread | 44.6 | 44.4 | 44.8 | 45.3 | 55.1 | 65.1 | 383 | 1,741 | 7,618 |
| Mojo BigBuffer, main thread | 47.4 | 47.8 | 47.7 | 48.7 | 55.9 | 66.2 | 424 | 491 | 1,565 |

## `--stress 0 --stress-nice 12`
DataPath shm: median of three invocations (range of the medians in brackets). Mojo and DataPath socket: one invocation each (a Mojo stress invocation at this placement takes 30–55 minutes).

p99.9 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm (median of 3) | 10.2 (10.2–10.6) | 10.1 (10.1–10.1) | 10.8 (10.4–10.9) | 13.2 (12.7–13.3) | 22.8 (21.4–36.3) | 50.8 (40.6–179) | 4,058 (3,726–4,088) | 4,207 (4,206–4,260) | 6,396 (5,382–6,480) |
| DataPath socket | 3,112 | 249 | 292 | 3,689 | 5,507 | 5,648 | 5,105 | 7,302 | 13,481 |
| Mojo raw pipe | 4,142 | 4,123 | 4,132 | 4,141 | 4,159 | 4,199 | 7,805 | 16,300 | 50,573 |
| Mojo bindings, IO thread | 4,181 | 4,173 | 4,167 | 4,164 | 4,199 | 4,249 | 8,055 | 18,561 | 56,097 |
| Mojo bindings, main thread | 7,579 | 7,852 | 7,867 | 7,853 | 7,997 | 8,127 | 10,082 | 19,179 | 53,033 |
| Mojo BigBuffer, main thread | 7,860 | 7,796 | 7,685 | 7,836 | 8,051 | 8,053 | 10,513 | 9,441 | 14,120 |

p99 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm (median of 3) | 6.54 (6.44–6.76) | 6.73 (6.16–6.92) | 6.93 (6.91–7.49) | 8.41 (8.18–8.72) | 12.3 (12.2–13.0) | 24.6 (23.9–25.4) | 74.2 (72.1–78.5) | 3,533 (3,445–4,017) | 4,279 (4,188–4,285) |
| DataPath socket | 94.6 | 84.4 | 89.1 | 144 | 248 | 2,305 | 3,928 | 4,525 | 10,459 |
| Mojo raw pipe | 290 | 330 | 343 | 356 | 425 | 874 | 4,959 | 13,961 | 47,458 |
| Mojo bindings, IO thread | 395 | 401 | 406 | 416 | 1,127 | 3,111 | 5,982 | 15,939 | 52,238 |
| Mojo bindings, main thread | 5,623 | 5,671 | 5,704 | 5,763 | 5,883 | 6,022 | 8,104 | 16,097 | 50,014 |
| Mojo BigBuffer, main thread | 5,672 | 5,706 | 5,728 | 5,735 | 5,879 | 6,057 | 8,229 | 8,094 | 10,911 |

p50 (µs):

| | 16 B | 64 B | 256 B | 1 KiB | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|---|---|---|---|
| DataPath shm (median of 3) | 3.19 (3.04–3.31) | 3.23 (3.15–3.42) | 3.48 (3.43–3.79) | 4.65 (4.44–4.97) | 8.44 (8.38–8.80) | 16.9 (16.2–17.4) | 49.8 (49.4–50.9) | 291 (282–295) | 867 (828–868) |
| DataPath socket | 29.6 | 30.1 | 32.6 | 36.8 | 71.2 | 151 | 267 | 560 | 2,088 |
| Mojo raw pipe | 171 | 193 | 209 | 216 | 256 | 297 | 1,172 | 7,180 | 36,057 |
| Mojo bindings, IO thread | 230 | 233 | 235 | 247 | 291 | 336 | 1,367 | 8,468 | 41,328 |
| Mojo bindings, main thread | 336 | 366 | 376 | 379 | 436 | 486 | 1,651 | 8,537 | 37,798 |
| Mojo BigBuffer, main thread | 372 | 380 | 382 | 386 | 439 | 493 | 1,838 | 1,949 | 5,737 |

Unpinned, with three invocations per variant, the same picture: Mojo main-thread bindings 6.0 ms p99.9 at 16 B under stress, raw pipe 0.2–0.4 ms (it depends on placement), DataPath shm 9.9 µs.

## Why
* **The IO thread to main thread hop.** Mojo reads every message on its IO thread and, for an interface bound elsewhere, posts a task to the bound sequence. Main-thread bindings therefore pay a thread wake-up on both sides of every round trip: idle that is the difference between 22 µs (raw pipe) and 45 µs (main thread) at p50. Under full load a woken thread waits for a scheduler timeslice, which is why main-thread bindings reach 5.6 ms at p99 and 7.6 ms at p99.9 for 16 B messages. This matches what users see in real browsers on a saturated machine; the benchmark itself only measures the IPC layer.
* **Socket wake-ups without a spin.** Even on the IO thread Mojo blocks in `epoll` after each message, so idle round trips cost a kernel wake-up per hop (22 µs p50 for the raw pipe vs 0.8–0.9 µs for DataPath over shared memory and 9.4 µs over its socket, which spins). Under load that wake-up waits for a timeslice: 4.1 ms p99.9.
* **Per-message allocation for inline payloads.** A 1 MiB inline message is allocated and freed by PartitionAlloc on every hop: about 1,800 page faults per message in each process, half the run spent in the kernel. The socket then carries it in pieces, with about 400 IO thread wake-ups per round trip. Together that holds inline messages at about 150 MiB/s per direction. Enabling `MojoUseEventFd`, `MojoIpczMemV2`, `MojoFixGeometricBufferGrowth` or a dynamic `MojoIpczParcelCapacityPolicy` made no measurable difference. Chromium's own `mojo_perftests` `MultiprocessPingPong` reports even slower round trips on this machine.
* **BigBuffer creates a region per message.** Above `BigBuffer::kMaxInlineBytes` (64 KiB; a message of exactly 64 KiB is still inline) the payload goes into a new shared-memory region, which is created, mapped, faulted in, sent as a handle and unmapped on every message. That is far cheaper than the inline path at 256 KiB and 1 MiB, but it is not a ring: DataPath reuses one mapped ring per direction, so the cost is the copy alone.
* **Same transport, still slower.** Over the same kind of Unix socket, DataPath's socket path is 2.5× below Mojo's raw pipe idle (13 vs 32 µs p99.9 at 16 B): one framed write and one read per message on a thread that spins, against ipcz's routing, validation and the wake-up above.

## Placement note
`--pin` pins each whole process to one CPU, for DataPath and Mojo alike, so Mojo's IO and main threads share that CPU. This is the same constraint DataPath's own benchmark runs under (its I/O thread and the main thread share the pinned CPU too). Unpinned, idle Mojo numbers are within a few microseconds of the pinned ones (raw pipe 27 vs 32 µs p99.9 at 16 B; main-thread bindings 53 vs 61 µs); under load the unpinned process can use idle hyper-threads and some runs come out lower.
