# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 Michael Fabian Dirks <info@xaymar.com>
# 
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
# 
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

################################################################################
# CMake Bootstrap
################################################################################
# CMake Setup
cmake_minimum_required(VERSION 3.1.0)

################################################################################
# Project Bootstrap
################################################################################
# Automatic Versioning
set(VERSION_MAJOR 0)
set(VERSION_MINOR 1)
set(VERSION_PATCH 0)
set(VERSION_TWEAK 0)
set(PROJECT_COMMIT "N/A")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git")
	set(GIT_RESULT "")
	set(GIT_OUTPUT "")
	execute_process(
		COMMAND git rev-list --count --topo-order ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}..HEAD
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		RESULT_VARIABLE GIT_RESULT
		OUTPUT_VARIABLE GIT_OUTPUT
		OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE ERROR_QUIET
	)
	if(GIT_RESULT EQUAL 0)
		set(VERSION_TWEAK ${GIT_OUTPUT})
	endif()
	execute_process(
		COMMAND git rev-parse HEAD
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		RESULT_VARIABLE GIT_RESULT
		OUTPUT_VARIABLE GIT_OUTPUT
		OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE ERROR_QUIET
	)
	if(GIT_RESULT EQUAL 0)
		set(PROJECT_COMMIT ${GIT_OUTPUT})
	endif()
endif()

# Define Project
project(
	datapath
	VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.${VERSION_TWEAK}
)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${PROJECT_SOURCE_DIR}/cmake/modules/")

# Check if we are compiling standalone
set(IsStandalone false)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
	set(IsStandalone true)
endif()

# Modules
include("util")

# Detect Build Type
if(IsStandalone)
	set(PropertyPrefix "")
else()
	set(PropertyPrefix "${PROJECT_NAME}_")
endif()

# Detect Architecture
math(EXPR BITS "8*${CMAKE_SIZEOF_VOID_P}")
if("${BITS}" STREQUAL "32")
	set(ARCH "x86")
else()
	set(ARCH "x64")
endif()

# Configure Version Header
configure_file(
	"${PROJECT_SOURCE_DIR}/cmake/version.hpp.in"
	"${PROJECT_BINARY_DIR}/include/version.hpp"
)

################################################################################
# Options
################################################################################
# Static or Dynamic?
option(${PropertyPrefix}MAKE_STATIC "Make Static Library" ON)
option(${PropertyPrefix}MAKE_DYNAMIC "Make Dynamic Library" OFF)
option(${PropertyPrefix}MAKE_MODULE "Make Module Library" OFF)

# Tests & Samples
option(${PropertyPrefix}BUILD_TESTS "Build Tests" ON)
option(${PropertyPrefix}BUILD_SAMPLES "Build Samples" ON)

################################################################################
# Source
################################################################################
# Public (exported with module)
set(PROJECT_PUBLIC
	"include/bitmask.hpp"
	"include/datapath.hpp"
	"include/error.hpp"
	"include/event.hpp"
	"include/isocket.hpp"
	"include/iserver.hpp"
	"include/itask.hpp"
	"include/waitable.hpp"
	"include/permissions.hpp"
	"include/threadpool.hpp"
)

set(PROJECT_PUBLIC_GENERATED
	"${PROJECT_BINARY_DIR}/include/version.hpp"
)

set(PROJECT_DATA
	"README.md"
	"LICENSE"
)

# Private (only compiled/used locally)
set(PROJECT_PRIVATE
	"source/threadpool.cpp"
)

# Libraries
set(PROJECT_LIBRARIES
)

# Defines
set(PROJECT_DEFINES
)

# Platforms
if(WIN32)
	# Windows
	list(APPEND PROJECT_LIBRARIES
		advapi32
	)

	list(APPEND PROJECT_DEFINES
		_CRT_SECURE_NO_WARNINGS
			WIN32_LEAN_AND_MEAN
			NOGPICAPMASKS
			NOVIRTUALKEYCODES
			NOWINMESSAGES
			NOWINSTYLES
			NOSYSMETRICS
			NOMENUS
			NOICONS
			NOKEYSTATES
			NOSYSCOMMANDS
			NORASTEROPS
			NOSHOWWINDOW
			NOATOM
			NOCLIPBOARD
			NOCOLOR
			NOCTLMGR
			NODRAWTEXT
			NOGDI
			NOKERNEL
			#NOUSER
			#NONLS
			NOMB
			NOMEMMGR
			NOMETAFILE
			NOMINMAX
			NOMSG
			NOOPENFILE
			NOSCROLL
			NOSERVICE
			NOSOUND
			NOTEXTMETRIC
			NOWH
			NOWINOFFSETS
			NOCOMM
			NOKANJI
			NOHELP
			NOPROFILER
			NODEFERWINDOWPOS
			NOMCX
			NOIME
			NOMDI
			NOINOUT
	)

	list(APPEND PROJECT_PRIVATE
		"source/windows/datapath.cpp"
		"source/windows/overlapped.hpp"
		"source/windows/overlapped.cpp"
		"source/windows/socket.hpp"
		"source/windows/socket.cpp"
		"source/windows/server.hpp"
		"source/windows/server.cpp"
		"source/windows/task.hpp"
		"source/windows/task.cpp"
		"source/windows/utility.hpp"
		"source/windows/waitable.cpp"
	)
elseif(APPLE)
	# MacOSX

	list(APPEND PROJECT_PRIVATE
	)
elseif("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")
	# Linux

	list(APPEND PROJECT_PRIVATE
	)
elseif("${CMAKE_SYSTEM_NAME}" MATCHES "FreeBSD")
	# FreeBSD

	list(APPEND PROJECT_PRIVATE
	)
endif()

# Grouping
source_group("Data Files" FILES $PROJECT_DATA)
source_group(TREE "${PROJECT_SOURCE_DIR}/source" PREFIX "Source" FILES ${PROJECT_PRIVATE})
source_group(TREE "${PROJECT_SOURCE_DIR}/include" PREFIX "Include" FILES ${PROJECT_PUBLIC})
source_group(TREE "${PROJECT_BINARY_DIR}" PREFIX "Generated" FILES ${PROJECT_PUBLIC_GENERATED})

################################################################################
# Building
################################################################################
# Library definition
if(${PropertyPrefix}MAKE_STATIC)
	add_library(${PROJECT_NAME} STATIC
		${PROJECT_PRIVATE}
		${PROJECT_PUBLIC}
		${PROJECT_PUBLIC_GENERATED}
		${PROJECT_DATA}
	)
elseif(${PropertyPrefix}MAKE_DYNAMIC)
	add_library(${PROJECT_NAME} SHARED
		${PROJECT_PRIVATE}
		${PROJECT_PUBLIC}
		${PROJECT_PUBLIC_GENERATED}
		${PROJECT_DATA}
	)
elseif(${PropertyPrefix}MAKE_MODULE)
	add_library(${PROJECT_NAME} MODULE
		${PROJECT_PRIVATE}
		${PROJECT_PUBLIC}
		${PROJECT_PUBLIC_GENERATED}
		${PROJECT_DATA}
	)
else()
	message(CRITICAL "Building nothing completed, aborting. Check MAKE_STATIC, MAKE_DYNAMIC and MAKE_DYNAMIC.")
	return()
endif()

# Includes
target_include_directories(${PROJECT_NAME}
	PRIVATE source
	PUBLIC include
)

# Defines
target_compile_definitions(${PROJECT_NAME}
	PRIVATE ${PROJECT_DEFINES}
)

# Linking Directories
link_directories(
)

# Linking
target_link_libraries(${PROJECT_NAME}
	${PROJECT_LIBRARIES}
)

# File Version
if(WIN32)
	set_target_properties(
		${PROJECT_NAME}
		PROPERTIES
		VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${PROJECT_VERSION_TWEAK}
		SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${PROJECT_VERSION_TWEAK}
	)
else()
	set_target_properties(
		${PROJECT_NAME}
		PROPERTIES
		VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${PROJECT_VERSION_TWEAK}
		SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${PROJECT_VERSION_TWEAK}
	)
endif()


################################################################################
# Samples
################################################################################
if(${PropertyPrefix}BUILD_SAMPLES)
	add_subdirectory(${PROJECT_SOURCE_DIR}/samples)
endif()

################################################################################
# Tests
################################################################################
if(${PropertyPrefix}BUILD_TESTS)
	add_subdirectory(${PROJECT_SOURCE_DIR}/tests)
endif()
