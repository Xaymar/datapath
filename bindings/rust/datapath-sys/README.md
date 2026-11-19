# com-xaymar-datapath-sys

Raw FFI bindings to the C API of [DataPath](https://github.com/Xaymar/datapath),
a low latency IPC library for local processes. Most users want the safe
[`com-xaymar-datapath`](https://crates.io/crates/com-xaymar-datapath) crate
instead.

The build script compiles the bundled C++ sources with CMake (a C++17
compiler and CMake 3.16+ are required) and links them statically. Set
`DATAPATH_LIB_DIR` to link a prebuilt library instead (`DATAPATH_LINK=static`
or `dylib`, default `dylib`), or `DATAPATH_SOURCE_DIR` to build another
checkout.

Licensed under BSD-3-Clause.
