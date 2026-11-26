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
	target_link_libraries(datapath-shm-benchmark PRIVATE datapath bcrypt advapi32 psapi)
	target_include_directories(datapath-shm-benchmark PRIVATE "${PROJECT_SOURCE_DIR}/source")
endif()

# PROTOTYPE: one-to-many broadcast over one shared-memory ring (Linux only),
# measured against N unicast sends through the library's shared memory.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	add_executable(datapath-broadcast-proto broadcast/broadcast_proto.cpp)
	target_link_libraries(datapath-broadcast-proto PRIVATE datapath)
endif()

add_executable(datapath-echo echo/main.cpp)
target_link_libraries(datapath-echo PRIVATE datapath)

# Comparison against other IPC libraries (downloads them at configure time).
option(DATAPATH_BUILD_COMPARE "Build benchmarks for other IPC libraries (libuv, cpp-ipc, Boost.Interprocess) for comparison." OFF)
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

	# cpp-ipc (libipc): shared-memory channels. No release since v1.4.1 carries
	# the MSVC/MinGW fixes, so this pins the latest master commit.
	set(LIBIPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(LIBIPC_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
	set(LIBIPC_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(cppipc
		GIT_REPOSITORY https://github.com/mutouyun/cpp-ipc.git
		GIT_TAG 2e28547cd32b22c2e1f2c85d22d0882810838503 # master, 2026-02-19
	)
	FetchContent_MakeAvailable(cppipc)

	add_executable(cppipc-benchmark ${BENCH_HARNESS} benchmark/transport_cppipc.cpp)
	target_link_libraries(cppipc-benchmark PRIVATE ipc datapath)
	target_include_directories(cppipc-benchmark PRIVATE "${PROJECT_SOURCE_DIR}/source")
	target_compile_definitions(cppipc-benchmark PRIVATE CPPIPC_VERSION="2e28547")

	# Boost.Interprocess (header-only): an installed Boost if there is one,
	# otherwise the Boost release archive with only Interprocess configured.
	find_package(Boost 1.80 CONFIG QUIET)
	if(Boost_FOUND)
		set(DATAPATH_BOOST_TARGET Boost::headers)
	else()
		set(BOOST_INCLUDE_LIBRARIES interprocess)
		set(BOOST_ENABLE_CMAKE ON)
		FetchContent_Declare(boost
			URL https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz
			DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		)
		FetchContent_MakeAvailable(boost)
		set(DATAPATH_BOOST_TARGET Boost::interprocess)
	endif()

	add_executable(boost-benchmark ${BENCH_HARNESS} benchmark/transport_boost.cpp)
	target_link_libraries(boost-benchmark PRIVATE ${DATAPATH_BOOST_TARGET} datapath)
	target_include_directories(boost-benchmark PRIVATE "${PROJECT_SOURCE_DIR}/source")
	if(UNIX AND NOT APPLE)
		target_link_libraries(boost-benchmark PRIVATE rt pthread)
	endif()
	if(WIN32)
		# On Windows Boost.Interprocess defaults to its generic (spin/yield based)
		# mutex and condition emulation; this variant uses the native Windows
		# primitives instead. Shared memory stays file-backed either way.
		add_executable(boost-native-benchmark ${BENCH_HARNESS} benchmark/transport_boost.cpp)
		target_link_libraries(boost-native-benchmark PRIVATE ${DATAPATH_BOOST_TARGET} datapath)
		target_include_directories(boost-native-benchmark PRIVATE "${PROJECT_SOURCE_DIR}/source")
		target_compile_definitions(boost-native-benchmark PRIVATE BOOST_INTERPROCESS_FORCE_NATIVE_EMULATION)
	endif()
endif()
