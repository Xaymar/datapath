# Mojo benchmark source

The Chromium-side half of the [Mojo comparison](../mojo.md): a ping-pong
benchmark that builds inside a Chromium checkout and prints the same table as
`samples/benchmark` (`size count min p50 p99 p99.9 max MiB/s p99.9 lo/hi`, plus
`cpus` lines).

| File | |
|---|---|
| `datapath_mojo_bench.cc` | harness and the four transports |
| `echo.mojom` | `Ping(array<uint8>)`, `PingBig(mojo_base.mojom.BigBuffer)`, `Cpus()`, `Quit()` |
| `BUILD.gn` | `mojom("echo_mojom")` and `executable("datapath_mojo_bench")` |

## Build
Measured with Chromium `a57585d9b72f6e40acafacf988161e0f70e74da5` (main, 2026-11-22).

```bash
fetch --nohooks --no-history chromium && cd src && gclient runhooks
mkdir mojo/datapath_bench && cp <datapath>/docs/comparisons/mojo/{BUILD.gn,echo.mojom,datapath_mojo_bench.cc} mojo/datapath_bench/
```
Add `"//mojo/datapath_bench:datapath_mojo_bench"` to the `deps` of `group("mojo")`
in `mojo/BUILD.gn` so gn loads the directory, then:
```bash
gn gen out/Release --args='is_debug=false is_component_build=false symbol_level=0 dcheck_always_on=false use_remoteexec=false treat_warnings_as_errors=false'
autoninja -C out/Release mojo/datapath_bench:datapath_mojo_bench
```
No `install-build-deps.sh` is needed; the bundled sysroot and clang are enough.

## Run
```bash
out/Release/datapath_mojo_bench --transport=raw|bindings|bigbuffer [--bind=main|io] \
    --sizes=16,64,256,1024,4096,16384,65536,262144,1048576 --runs=5 --pin=1,5 \
    [--stress=0 --stress-nice=12] [--enable-features=...]
```
- `raw`: message pipe through the C API; a trap fires on Mojo's IO thread and
  the handler reads (no copy) and writes the next message (one copy) in place.
- `bindings`: `mojo::Remote` / `mojo::Receiver`, bound on the main thread
  (`--bind=main`, how browser code uses Mojo) or on the IO thread (`--bind=io`).
- `bigbuffer`: like `bindings` with `mojo_base.mojom.BigBuffer`; payloads above
  64 KiB travel in a new shared-memory region per message. Both sides copy, like
  DataPath's benchmark.
- `--pin C,S` pins the whole client process to CPU C and the server to S (the
  stress process stays free), like `datapath-benchmark --pin`.

Methodology is copied from `samples/benchmark/main.cpp`: 1 s 64 B warm-up,
count = clamp(100000, 50, 2 GiB / size), count/20 warm-up per run, median of
`--runs`, same percentile rule, same stress loop and busy-core report.
