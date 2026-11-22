# Benchmark harness shared by every transport; each executable links one transport.
set(BENCH_HARNESS
	benchmark/main.cpp
	benchmark/bench.hpp
	benchmark/clock.hpp
	benchmark/stats.hpp
)

add_executable(datapath-benchmark ${BENCH_HARNESS} benchmark/transport_datapath.cpp
	benchmark/matrix.cmd
	benchmark/matrix.sh
)
target_link_libraries(datapath-benchmark PRIVATE datapath)

# The same harness over the C API, to measure any core that exports datapath.h.
add_executable(datapath-benchmark-c ${BENCH_HARNESS} benchmark/transport_datapath_c.cpp)
target_link_libraries(datapath-benchmark-c PRIVATE datapath)

# PROTOTYPE: shared-memory transport (Windows only), to measure against the pipe.
if(WIN32)
	add_executable(datapath-shm-benchmark ${BENCH_HARNESS} benchmark/transport_shm.cpp
		"${PROJECT_SOURCE_DIR}/source/windows/path.cpp"
		"${PROJECT_SOURCE_DIR}/source/windows/security.cpp"
	)
	# Linked only for its option defaults; the transport compiles the two
	# helpers above itself so it also builds against a shared library.
	target_link_libraries(datapath-shm-benchmark PRIVATE datapath bcrypt advapi32)
	target_include_directories(datapath-shm-benchmark PRIVATE "${PROJECT_SOURCE_DIR}/source")
endif()

add_executable(datapath-echo echo/main.cpp)
target_link_libraries(datapath-echo PRIVATE datapath)

# Comparison against other IPC libraries (downloads them at configure time).
option(DATAPATH_BUILD_COMPARE "Build benchmarks for other IPC libraries (libuv) for comparison." OFF)
if(DATAPATH_BUILD_COMPARE)
	enable_language(C)
	include(FetchContent)
	set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
	set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(libuv
		GIT_REPOSITORY https://github.com/libuv/libuv.git
		GIT_TAG v1.52.1
		GIT_SHALLOW TRUE
	)
	FetchContent_MakeAvailable(libuv)

	add_executable(libuv-benchmark ${BENCH_HARNESS} benchmark/transport_libuv.cpp)
	# DataPath is linked only for its option defaults and frame parser headers.
	target_link_libraries(libuv-benchmark PRIVATE uv_a datapath)
	target_include_directories(libuv-benchmark PRIVATE "${PROJECT_SOURCE_DIR}/source")
endif()
