# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)

# Versioning: ABI version (bump on any binary-incompatible change, and reset
# the patch version to 0) and patch version (bump on compatible changes).
set(DATAPATH_VERSION_ABI 2)
set(DATAPATH_VERSION_PATCH 0)

project(datapath
	VERSION ${DATAPATH_VERSION_ABI}.${DATAPATH_VERSION_PATCH}
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

set(CMAKE_CXX_STANDARD 17)
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
target_sources(datapath PRIVATE ${DATAPATH_PUBLIC_HEADERS})

target_include_directories(datapath
	PUBLIC
		"$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
		"$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/include>"
		"$<INSTALL_INTERFACE:include>"
)
set_target_properties(datapath PROPERTIES
	VERSION ${DATAPATH_VERSION_ABI}.${DATAPATH_VERSION_PATCH}
	SOVERSION ${DATAPATH_VERSION_ABI}
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
install(EXPORT datapath-targets NAMESPACE datapath:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/datapath)

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
