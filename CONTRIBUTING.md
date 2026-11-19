# Contributing to DataPath
Thanks for helping out. This file covers building, testing and benchmarking DataPath. For what the library does and how to use it, see [README.md](README.md).

## Building
Requires CMake 3.16+ and a C++17 compiler (MSVC 2019+, GCC 9+, Clang 10+).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```
```bash
cmake --build build --config Release
```
```bash
ctest --test-dir build -C Release --output-on-failure
```

Options: `DATAPATH_SHARED` (shared library, default `OFF`), `DATAPATH_BUILD_TESTS` and `DATAPATH_BUILD_SAMPLES` (default `ON` for top-level builds). The `datapath::datapath` target can be used through `add_subdirectory`.

The shared library exports only what public headers mark with `DATAPATH_API` (from the generated `datapath/export.h`); everything else stays hidden. Mark any new public function or class with it. Tests that reach into internals (`source/`) pass `INTERNAL` to `datapath_add_test` so they still link in a shared build.

Cross-compiling for Windows from Linux with mingw-w64 (tests run through Wine if it's installed):
```bash
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake
```

## Testing
Run the whole suite with `ctest` as shown above before sending a change. Unix-only and Windows-only tests (`tests/test_posix.cpp` for Linux and macOS, `tests/test_windows_security.cpp`) build only on their platforms, so a change to a platform backend should be tested on that platform. On Linux, an AddressSanitizer build catches most lifetime bugs in the handler and close paths:
```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
```

## Benchmarking
`datapath-benchmark local` starts an echo server in a second process and runs the client against it:
```bash
./build/samples/datapath-benchmark local
```
On Windows, run `build\samples\Release\datapath-benchmark.exe local`. Useful variations:
* `--spin 0` measures without busy-polling.
* `--load` adds a 1 MiB bulk stream on a second connection and reports its throughput.
* `--clients 8` runs eight concurrent ping-pong connections.
* `--threads 2` uses two I/O threads per process.
* `--threads auto` uses one I/O thread per physical core (`context_options::io_threads = 0`). Compare it with `--ideal-core off` on Windows to see what pairing threads with cores does.
* `--stress 0` loads every logical core from a separate process; on Linux, `--stress-nice N` runs that process at nice N to test the priority gap.
* `--sizes 8,64,4096` picks the message sizes. The default is 8 B to 64 KiB; add `1048576,16777216` to measure bulk-sized messages.

You can also run `server <name>` and `client <name>` separately, in two terminals.

Single runs are noisy at microsecond resolution. `--runs 5` repeats every size five times and reports the median, plus the lowest and highest p99.9 of any run. `samples\benchmark\matrix.cmd` (Windows) and `samples/benchmark/matrix.sh` (Linux) run every option combination that way and collect the output in `benchmark-results.txt`.

For stable numbers on Windows, use the High Performance power plan. Run from a foreground console and close CPU-heavy applications.

### Comparing with libuv
Configure with `-DDATAPATH_BUILD_COMPARE=ON` to also build `libuv-benchmark`; CMake downloads libuv 1.52.1. It runs the same harness over libuv named pipes / Unix sockets: same framing, clock, sizes, `--runs`, `--stress`. It also uses the same `--spin`, `--priority` and `--mmcss` policy on its loop thread, so both libraries can be compared with identical settings. libuv itself has none of these, so use `--spin 0 --no-mmcss --priority 0` to measure libuv as it ships. The matrix scripts include libuv runs when the executable exists.

When you report numbers, lead with p99.9, say which machine, OS and power plan they came from, and give the flags. A p50 improvement that costs tail latency is a regression for this project.

## Python bindings
The bindings live in `python/` (nanobind, built by scikit-build-core from `pyproject.toml` in the repository root). Build and test them with:
```bash
pip install . pytest
```
```bash
pytest python/tests
```
For quicker iteration, configure CMake with `-DDATAPATH_BUILD_PYTHON=ON`; the build tree then has an importable package in `build/python/package`, so `PYTHONPATH=build/python/package pytest python/tests` tests it without installing. Release wheels are built by `.github/workflows/python.yml` with cibuildwheel (settings in `pyproject.toml`); it only uploads to PyPI when started by hand with `publish` set.

`python/benchmarks/pingpong.py local` measures Python round trips; add `--cpp-server build/samples/datapath-benchmark` to echo from the C++ server, and `--mode callback` to receive in callbacks instead of `recv()`.

## Pull requests
* Keep a pull request to one change, and say in the description what you measured before and after if it touches the data path.
* Format C++ changes with the repository's `.clang-format`.
* By contributing you agree that your contribution is licensed under the project's license, the BSD 3-Clause License.
