<!-- The "How it compares" block on the website's landing page. Links are relative to this file. Kept apart from README.md, which is only for the GitHub repository page. -->

Full round trip between two processes, p99.9 in µs, worst-case placement (client and server on different physical cores and, where the machine has them, different CCDs), median of 5 runs. Stress is one busy thread per logical CPU in a third process (at nice 12 on Linux); DataPath over shared memory and cpp-ipc under stress are the median of three invocations, other stress cells one. cpp-ipc and Boost.Interprocess are as shipped, measured 2026-11-25; "did not finish" means not even 16 B completed in 30 minutes.

**Linux**, AMD EPYC 7313P (8 cores, two CCDs), `--pin 1,5`:

| | 64 B idle | 4 KiB idle | 64 KiB idle | 1 MiB idle | 64 B stress | 4 KiB stress |
|---|---|---|---|---|---|---|
| **DataPath, shared memory** | **1.4** | **3.4** | **22** | **321** | **10** | **23** |
| cpp-ipc (shared memory) | 4.7 | 8.2 | 29 | 341 | 5,758 | 8,015 |
| Boost.Interprocess `message_queue` | 32 | 41 | 128 | 1,155 | 464 | 313 |
| DataPath, Unix socket | 13 | 19 | 60 | 479 | 249 | 5,507 |
| libuv + DataPath's spin/priority | 14 | 20 | 56 | 541 | 3,494 | 4,924 |
| libuv as shipped | 23 | 28 | 64 | 557 | 4,070 | 4,076 |
| Chromium Mojo, bindings (main thread) | 62 | 73 | 439 | 8,704 | 7,852 | 7,997 |

**Windows**, AMD Ryzen 9 7940HS (8 cores, one CCD), `--pin 8,12`:

| | 64 B idle | 4 KiB idle | 64 KiB idle | 1 MiB idle | 64 B stress | 4 KiB stress |
|---|---|---|---|---|---|---|
| **DataPath, shared memory** | **2.2** | **4.1** | **19** | **130** | **5.8** | **26** |
| cpp-ipc (shared memory) | 18 | 31 | 153 | 569 | did not finish | did not finish |
| Boost.Interprocess `message_queue` | 1.1 | 4.1 | 55 | 6,310 | did not finish | did not finish |
| DataPath, named pipe | 6.9 | 17 | 50 | 754 | 40 | 113 |
| libuv + DataPath's spin/priority | 15 | 22 | 74 | 9,659 | 104 | 187 |
| libuv as shipped | 31 | 35 | 66 | 1,075 | 265 | 401 |

Against [libuv](../../docs/comparisons/libuv.md), DataPath's socket and pipe paths win through scheduling policy (busy-poll and priority), and shared memory is a further 3–10× for small messages. The two shared-memory libraries fall apart under load: [cpp-ipc](../../docs/comparisons/cpp-ipc.md) is about 3× DataPath idle for small messages on Linux but 4–8 ms at p99.9 with every core busy, and [Boost.Interprocess](../../docs/comparisons/boost-interprocess.md) is 15–20× DataPath on Linux. On Windows Boost's default spin-lock emulation is the fastest idle option for small messages, and neither library finished a 16 B run under full load in 30 minutes. Against [Chromium's Mojo](../../docs/comparisons/mojo.md) (Linux) it is 20–50× lower idle and 400–800× lower under full load for small messages. Full tables (p50, p99, p99.9), the methodology and the reasons are in [docs/comparisons](../../docs/comparisons/README.md).
