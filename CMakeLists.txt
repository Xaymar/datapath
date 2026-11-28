# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)

# Semantic Versioning (https://semver.org): bump MAJOR on any API- or
# ABI-incompatible change, MINOR on compatible additions, PATCH on compatible
# fixes. Every binding and package uses this same version.
set(DATAPATH_VERSION_MAJOR 1)
set(DATAPATH_VERSION_MINOR 0)
set(DATAPATH_VERSION_PATCH 1)
if(DATAPATH_VERSION_MAJOR GREATER 255 OR DATAPATH_VERSION_MINOR GREATER 65535 OR DATAPATH_VERSION_PATCH GREATER 255)
	message(FATAL_ERROR "DataPath: version parts must fit (major << 24) | (minor << 8) | patch")
endif()

project(datapath
	VERSION ${DATAPATH_VERSION_MAJOR}.${DATAPATH_VERSION_MINOR}.${DATAPATH_VERSION_PATCH}
	DESCRIPTION "High performance, low latency IPC library"
	LANGUAGES C CXX
)

if("${CMAKE_SOURCE_DIR}" STREQUAL "${PROJECT_SOURCE_DIR}")
	set(DATAPATH_TOPLEVEL ON)
else()
	set(DATAPATH_TOPLEVEL OFF)
endif()

option(DATAPATH_SHARED "Build a shared library instead of a static one." OFF)
option(DATAPATH_BUILD_SAMPLES "Build samples and the benchmark." ${DATAPATH_TOPLEVEL})
option(DATAPATH_BUILD_TESTS "Build tests." ${DATAPATH_TOPLEVEL})
option(DATAPATH_BUILD_PYTHON "Build the Python extension module (normally driven by pyproject.toml)." OFF)

# The library builds with C++11 or newer; newer standards let it use newer
# standard library facilities (see source/common/compat.hpp). An explicit
# CMAKE_CXX_STANDARD picks the default. Tests, samples and the Python module
# need C++17 and use the larger of 17 and this.
if(DEFINED CMAKE_CXX_STANDARD)
	set(_datapath_default_std ${CMAKE_CXX_STANDARD})
else()
	set(_datapath_default_std 17)
endif()
set(DATAPATH_CXX_STANDARD ${_datapath_default_std} CACHE STRING "C++ standard the library is built with (11 or newer).")
set_property(CACHE DATAPATH_CXX_STANDARD PROPERTY STRINGS 11 14 17 20 23 26)
if(DATAPATH_CXX_STANDARD LESS 11 OR DATAPATH_CXX_STANDARD EQUAL 98)
	message(FATAL_ERROR "DataPath: DATAPATH_CXX_STANDARD must be 11 or newer, got ${DATAPATH_CXX_STANDARD}")
endif()
if(DATAPATH_CXX_STANDARD LESS 17)
	set(CMAKE_CXX_STANDARD 17)
else()
	set(CMAKE_CXX_STANDARD ${DATAPATH_CXX_STANDARD})
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

configure_file(
	"${PROJECT_SOURCE_DIR}/cmake/version.hpp.in"
	"${PROJECT_BINARY_DIR}/generated/include/datapath/version.hpp"
	@ONLY
)
if(DATAPATH_SHARED)
	set(DATAPATH_BUILT_STATIC 0)
else()
	set(DATAPATH_BUILT_STATIC 1)
endif()
configure_file(
	"${PROJECT_SOURCE_DIR}/cmake/export.h.in"
	"${PROJECT_BINARY_DIR}/generated/include/datapath/export.h"
	@ONLY
)

set(DATAPATH_PUBLIC_HEADERS
	"${PROJECT_BINARY_DIR}/generated/include/datapath/export.h"
	"${PROJECT_BINARY_DIR}/generated/include/datapath/version.hpp"
	include/datapath/context.hpp
	include/datapath/datapath.h
	include/datapath/datapath.hpp
	include/datapath/error.hpp
	include/datapath/options.hpp
	include/datapath/permissions.hpp
	include/datapath/server.hpp
	include/datapath/socket.hpp
)
set(DATAPATH_SOURCES
	source/common/api.cpp
	source/common/c_api.cpp
	source/common/connection_base.hpp
	source/common/detail.hpp
	source/common/endian.hpp
	source/common/framing.hpp
	source/common/handshake.hpp
	source/common/send_queue.cpp
	source/common/send_queue.hpp
	source/common/shm_ring.cpp
	source/common/shm_ring.hpp
	source/common/spin.hpp
)
if(WIN32)
	list(APPEND DATAPATH_SOURCES
		source/windows/iocp_context.cpp
		source/windows/iocp_context.hpp
		source/windows/path.cpp
		source/windows/path.hpp
		source/windows/pipe_connection.cpp
		source/windows/pipe_connection.hpp
		source/windows/pipe_server.cpp
		source/windows/pipe_server.hpp
		source/windows/security.cpp
		source/windows/security.hpp
		source/windows/shm.cpp
		source/windows/shm.hpp
		source/windows/win.hpp
	)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID OR APPLE)
	# One backend for Unix domain sockets; the poller and the OS-specific parts
	# (sockets flags, thread priority, core count) differ.
	list(APPEND DATAPATH_SOURCES
		source/posix/event_context.cpp
		source/posix/event_context.hpp
		source/posix/path.cpp
		source/posix/path.hpp
		source/posix/platform.hpp
		source/posix/shm.cpp
		source/posix/shm.hpp
		source/posix/uds_connection.cpp
		source/posix/uds_connection.hpp
		source/posix/uds_server.cpp
		source/posix/uds_server.hpp
	)
	if(APPLE)
		list(APPEND DATAPATH_SOURCES
			source/posix/event_loop_kqueue.cpp
			source/posix/platform_apple.cpp
		)
	else()
		list(APPEND DATAPATH_SOURCES
			source/posix/event_loop_epoll.cpp
			source/posix/platform_linux.cpp
		)
	endif()
else()
	message(FATAL_ERROR "DataPath: unsupported platform ${CMAKE_SYSTEM_NAME}")
endif()

# Everything is compiled once into datapath_objects. The library is built from
# those objects; so is datapath_internal, which the unit tests link to reach
# internals that a shared library does not export.
add_library(datapath_objects OBJECT ${DATAPATH_PUBLIC_HEADERS} ${DATAPATH_SOURCES})
target_include_directories(datapath_objects
	PUBLIC
		"$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
		"$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>"
	PRIVATE
		"${PROJECT_SOURCE_DIR}/source"
)
# Only symbols marked DATAPATH_API (include/datapath/export.h) are exported.
target_compile_definitions(datapath_objects PRIVATE DATAPATH_BUILDING)
set_target_properties(datapath_objects PROPERTIES
	CXX_STANDARD ${DATAPATH_CXX_STANDARD}
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON
	POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
	target_compile_options(datapath_objects PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
else()
	target_compile_options(datapath_objects PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
	target_compile_definitions(datapath_objects PRIVATE
		WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00
	)
	set(DATAPATH_PLATFORM_LIBS advapi32 kernel32)
else()
	find_package(Threads REQUIRED)
	set(DATAPATH_PLATFORM_LIBS Threads::Threads)
endif()
target_link_libraries(datapath_objects PRIVATE ${DATAPATH_PLATFORM_LIBS})

if(DATAPATH_SHARED)
	add_library(datapath SHARED $<TARGET_OBJECTS:datapath_objects>)
	target_link_libraries(datapath PRIVATE ${DATAPATH_PLATFORM_LIBS})
else()
	add_library(datapath STATIC $<TARGET_OBJECTS:datapath_objects>)
	# A static library carries its dependencies to whoever links it.
	target_link_libraries(datapath PUBLIC ${DATAPATH_PLATFORM_LIBS})
endif()
add_library(datapath::datapath ALIAS datapath)
set_target_properties(datapath PROPERTIES CXX_STANDARD ${DATAPATH_CXX_STANDARD})
# The public C++ headers need C++11.
target_compile_features(datapath INTERFACE cxx_std_11)
target_sources(datapath PRIVATE ${DATAPATH_PUBLIC_HEADERS})

target_include_directories(datapath
	PUBLIC
		"$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
		"$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>"
		"$<INSTALL_INTERFACE:include>"
)
set_target_properties(datapath PROPERTIES
	VERSION ${DATAPATH_VERSION_MAJOR}.${DATAPATH_VERSION_MINOR}.${DATAPATH_VERSION_PATCH}
	SOVERSION ${DATAPATH_VERSION_MAJOR}
)

if(MINGW)
	# Self-contained executables (no libstdc++/winpthread DLLs needed under Wine or on Windows).
	target_link_options(datapath PUBLIC -static -static-libgcc -static-libstdc++)
endif()

include(GNUInstallDirs)
install(TARGETS datapath EXPORT datapath-targets
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/datapath DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES
	"${PROJECT_BINARY_DIR}/generated/include/datapath/export.h"
	"${PROJECT_BINARY_DIR}/generated/include/datapath/version.hpp"
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/datapath
)
set(DATAPATH_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/datapath")
install(EXPORT datapath-targets NAMESPACE datapath:: DESTINATION ${DATAPATH_CMAKE_DIR})

# find_package(datapath) support: config + version file. SameMajorVersion is
# SemVer: any 1.x satisfies a request for 1.y with x >= y.
include(CMakePackageConfigHelpers)
configure_package_config_file(
	"${PROJECT_SOURCE_DIR}/cmake/datapath-config.cmake.in"
	"${PROJECT_BINARY_DIR}/datapath-config.cmake"
	INSTALL_DESTINATION ${DATAPATH_CMAKE_DIR}
)
write_basic_package_version_file(
	"${PROJECT_BINARY_DIR}/datapath-config-version.cmake"
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion
)
install(FILES
	"${PROJECT_BINARY_DIR}/datapath-config.cmake"
	"${PROJECT_BINARY_DIR}/datapath-config-version.cmake"
	DESTINATION ${DATAPATH_CMAKE_DIR}
)

# pkg-config, for non-CMake builds. A static library also needs the C++
# runtime and threads when it is linked into a C program.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
	set(DATAPATH_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
	set(DATAPATH_PC_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
	set(DATAPATH_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
	set(DATAPATH_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()
set(DATAPATH_PC_LIBS_PRIVATE "")
if(WIN32)
	set(DATAPATH_PC_LIBS_PRIVATE "-ladvapi32 -lkernel32")
else()
	if(APPLE OR CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND "c++" IN_LIST CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
		set(DATAPATH_PC_LIBS_PRIVATE "-lc++")
	else()
		set(DATAPATH_PC_LIBS_PRIVATE "-lstdc++")
	endif()
	string(APPEND DATAPATH_PC_LIBS_PRIVATE " -pthread")
endif()
configure_file("${PROJECT_SOURCE_DIR}/cmake/datapath.pc.in" "${PROJECT_BINARY_DIR}/datapath.pc" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/datapath.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/datapath)

if(DATAPATH_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
if(DATAPATH_BUILD_PYTHON)
	add_subdirectory(python)
endif()
if(DATAPATH_BUILD_SAMPLES AND EXISTS "${PROJECT_SOURCE_DIR}/samples/CMakeLists.txt")
	add_subdirectory(samples)
endif()
