# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Michael Fabian Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)

# Versioning: tweak = commits since the version tag, if available.
set(VERSION_MAJOR 0)
set(VERSION_MINOR 2)
set(VERSION_PATCH 0)
set(VERSION_TWEAK 0)
set(PROJECT_COMMIT "N/A")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git")
	execute_process(
		COMMAND git rev-parse HEAD
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		RESULT_VARIABLE GIT_RESULT
		OUTPUT_VARIABLE GIT_OUTPUT
		OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
	)
	if(GIT_RESULT EQUAL 0)
		set(PROJECT_COMMIT ${GIT_OUTPUT})
	endif()
endif()

project(datapath
	VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.${VERSION_TWEAK}
	DESCRIPTION "High performance, low latency IPC library"
	LANGUAGES CXX
)

# Optional clang tooling submodule.
if(EXISTS "${PROJECT_SOURCE_DIR}/cmake/clang/Clang.cmake")
	list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/clang")
	include("Clang")
endif()

if("${CMAKE_SOURCE_DIR}" STREQUAL "${PROJECT_SOURCE_DIR}")
	set(DATAPATH_TOPLEVEL ON)
else()
	set(DATAPATH_TOPLEVEL OFF)
endif()

option(DATAPATH_SHARED "Build a shared library instead of a static one." OFF)
option(DATAPATH_BUILD_SAMPLES "Build samples and the benchmark." ${DATAPATH_TOPLEVEL})
option(DATAPATH_BUILD_TESTS "Build tests." ${DATAPATH_TOPLEVEL})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

configure_file(
	"${PROJECT_SOURCE_DIR}/cmake/version.hpp.in"
	"${PROJECT_BINARY_DIR}/generated/version.hpp"
)

set(DATAPATH_PUBLIC_HEADERS
	include/datapath/context.hpp
	include/datapath/datapath.hpp
	include/datapath/error.hpp
	include/datapath/options.hpp
	include/datapath/permissions.hpp
	include/datapath/server.hpp
	include/datapath/socket.hpp
)
set(DATAPATH_SOURCES
	source/common/api.cpp
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
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
	list(APPEND DATAPATH_SOURCES
		source/linux/epoll_context.cpp
		source/linux/epoll_context.hpp
		source/linux/path.cpp
		source/linux/path.hpp
		source/linux/uds_connection.cpp
		source/linux/uds_connection.hpp
		source/linux/uds_server.cpp
		source/linux/uds_server.hpp
	)
else()
	message(FATAL_ERROR "DataPath: unsupported platform ${CMAKE_SYSTEM_NAME}")
endif()

if(DATAPATH_SHARED)
	add_library(datapath SHARED ${DATAPATH_PUBLIC_HEADERS} ${DATAPATH_SOURCES})
else()
	add_library(datapath STATIC ${DATAPATH_PUBLIC_HEADERS} ${DATAPATH_SOURCES})
endif()
add_library(datapath::datapath ALIAS datapath)

target_include_directories(datapath
	PUBLIC
		"$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
		"$<INSTALL_INTERFACE:include>"
	PRIVATE
		"${PROJECT_SOURCE_DIR}/source"
		"${PROJECT_BINARY_DIR}/generated"
)
set_target_properties(datapath PROPERTIES
	CXX_VISIBILITY_PRESET hidden
	POSITION_INDEPENDENT_CODE ON
	WINDOWS_EXPORT_ALL_SYMBOLS ON
)

if(MSVC)
	target_compile_options(datapath PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
else()
	target_compile_options(datapath PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
	target_compile_definitions(datapath PRIVATE
		WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00
	)
	target_link_libraries(datapath PRIVATE advapi32 kernel32)
else()
	find_package(Threads REQUIRED)
	target_link_libraries(datapath PUBLIC Threads::Threads)
endif()

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
install(EXPORT datapath-targets NAMESPACE datapath:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/datapath)

if(DATAPATH_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
if(DATAPATH_BUILD_SAMPLES AND EXISTS "${PROJECT_SOURCE_DIR}/samples/CMakeLists.txt")
	add_subdirectory(samples)
endif()
