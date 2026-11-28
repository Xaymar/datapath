# com-xaymar-datapath-sys
DataPath is an IPC library for fast and predictable messaging between processes on the same machine made in C++11 (or newer). Designed from the ground up to care about the worst case it is able to outperform or match existing solutions such as boost interprocess, libuv, Mojo and others. It allows for packets from a few bytes all the way to many megabytes between one server and any number of clients. The exact implementation varies by platform and makes use of the best measured available systems.

These are the raw FFI bindings to the C API of [DataPath](https://github.xaymar.com/datapath/). Most users want the safe [`com-xaymar-datapath`](https://crates.io/crates/com-xaymar-datapath) crate instead, which also re-exports this one as `datapath::sys`.

## Usage
```bash
cargo add com-xaymar-datapath-sys --rename datapath_sys
```

Every function and type matches `include/datapath/datapath.h` one to one, so the C API documentation applies as is.

### Building
* **From source (default).** The build script compiles the bundled C++ sources with CMake 3.16+ and a C++17 compiler and links them statically.
* **Prebuilt.** Set `DATAPATH_LIB_DIR` to a directory with a prebuilt library, such as one from the [GitHub Releases](https://github.com/Xaymar/datapath/releases) for Windows, Linux or macOS. `DATAPATH_LINK` picks `static` or `dylib` (default `dylib`).
* **Another checkout.** Set `DATAPATH_SOURCE_DIR` to build a different DataPath source tree.

## License
BSD 3-Clause License, see `LICENSE`.
