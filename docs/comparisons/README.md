# Comparisons

How DataPath compares with other IPC stacks, measured with one harness so the
numbers line up:

* [libuv](libuv.md): named pipes / Unix sockets through libuv 1.52.1, as shipped and with DataPath's scheduling policy.
* [Chromium Mojo](mojo.md): raw message pipes, mojom bindings and BigBuffer.
* [cpp-ipc](cpp-ipc.md): shared-memory channels (libipc), as shipped and with DataPath's spin.
* [Boost.Interprocess](boost-interprocess.md): `message_queue`, as shipped and with DataPath's spin; on Windows also with native sync primitives.
* [Memory bandwidth and placement](memory-bandwidth.md): why large messages cost what they cost, and how much CPU placement matters.

## Methodology
* **Round trip.** A client sends a message of N bytes, a server in a second process echoes it, and the client times send to receive with `CLOCK_MONOTONIC` (`QueryPerformanceCounter` on Windows). The next message is sent from the receive callback. Sizes 16 B to 1 MiB; each size runs `min(100000, 2 GiB / size)` round trips (at least 50) after a 5% warm-up, so 256 KiB gets 8,192 and 1 MiB 2,048.
* **Runs.** `--runs 5`: each row is the median of five runs, and "lo–hi" is the lowest and highest p99.9 of any run. Percentiles are taken from the sorted samples at index `round(q * (n - 1))`.
* **Worst-case placement.** Client and server are pinned (`--pin C,S`) to different physical cores, on different CCDs (L3 domains) and different NUMA nodes where the machine has them. Best-case placement is easy to win by chance; the worst case is what a library has to handle. Same-CCD numbers appear only as a labelled reference.
* **Idle and stress.** Idle means nothing else runs. Stress (`--stress 0`) starts a separate process with one busy thread per logical CPU (integer work plus an 8 MiB streaming pass, to evict caches). On Linux it runs at nice 12 (`--stress-nice 12`), the gap `priority` 2 gives where the system allows it; at equal nice every variant measured 4–8 ms, one scheduler timeslice. DataPath over shared memory under stress is the median of three invocations (15 runs); single-invocation rows say so.
* **p99.9 first.** The headline metric is p99.9: one round trip in a thousand is slower. A p50 gain that costs tail latency is a regression.

## Machines
| | Linux: merenka-gpu0 | Windows: Irkela |
|---|---|---|
| CPU | AMD EPYC 7313P, container limited to 8 cores / 16 threads | AMD Ryzen 9 7940HS, 8 cores / 16 threads |
| Topology | CPUs 0–7 and 16–23; two CCDs (L3: 0–3,16–19 and 4–7,20–23), one NUMA node | one CCD, one NUMA node |
| Placement | `--pin 1,5`: different cores, different CCDs | `--pin 8,12`: different cores (the worst case a single-CCD machine has) |
| OS, compiler | Linux 6.12, GCC 15.2 (Mojo: Chromium's clang) | Windows 10 22H2, Balanced plan, MSVC 19.51 |
| Measured | 2026-11-23, DataPath `cdc663a` | 2026-11-23, DataPath `df30376` |

On Linux the container's `RLIMIT_NICE` is 0, so DataPath can't raise its I/O thread priority; the stress process is lowered instead. `std::thread::hardware_concurrency()` reports 32 there although the container has 16 CPUs, so `--stress 0` starts 32 threads; they keep about 14–15 CPUs busy (the harness warns "under half its CPU time", which is that artifact, not a light load).

## Reproducing
`samples/benchmark` builds `datapath-benchmark` and, with `-DDATAPATH_BUILD_COMPARE=ON`, `libuv-benchmark`, `cppipc-benchmark` and `boost-benchmark` (plus `boost-native-benchmark` on Windows); [CONTRIBUTING.md](../../CONTRIBUTING.md#benchmarking) lists the options. The Mojo benchmark lives in a Chromium checkout; its source and build steps are in [mojo/](mojo/README.md). `membw.cpp` is the memcpy benchmark behind the memory-bandwidth note.
