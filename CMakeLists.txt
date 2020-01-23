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

#================================================================================#
# Define Project
#================================================================================#
cmake_minimum_required(VERSION 3.15.0)

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
	DataPath
	VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.${VERSION_TWEAK}
)
set(PROJECT_FULL_NAME "DataPath IPC Library")
set(PROJECT_DESCRIPTION "High Performance Synchronous and Asynchronous IPC Library")
set(PROJECT_AUTHORS "Michael Fabian 'Xaymar' Dirks <info@xaymar.com>")
set(PROJECT_COPYRIGHT_YEARS "2018 - 2020")

#================================================================================#
# CMake Tools & Modules
#================================================================================#

# CMake Modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/clang")

include("util")
include("Clang")

# Detect Build Type
if(NOT "${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
	set(PropertyPrefix "${PROJECT_NAME}_")
else()
	set(PropertyPrefix "")
endif()

# Detect Architecture
math(EXPR BITS "8*${CMAKE_SIZEOF_VOID_P}")
if("${BITS}" STREQUAL "32")
	set(ARCH "x86")
else()
	set(ARCH "x64")
endif()

#================================================================================#
# Options
#================================================================================#
# Static or Dynamic?
option(${PropertyPrefix}MAKE_DYNAMIC "Create dynamically linked library instead of static library." OFF)
option(${PropertyPrefix}MAKE_MODULE "Create dynamically linked module instead of dynamically linked library." OFF)
option(${PropertyPrefix}BUILD_SAMPLES "Build Samples" ON)

#================================================================================#
# Sources
#================================================================================#

# Configure Version Header
configure_file(
	"${PROJECT_SOURCE_DIR}/cmake/version.hpp.in"
	"${PROJECT_BINARY_DIR}/generated/version.hpp"
)

# Source Files
set(PROJECT_PUBLIC "")
list(APPEND PROJECT_PUBLIC
	"include/datapath.hpp"
	"include/error.hpp"
	"include/bitmask.hpp"
	"include/event.hpp"
	"include/isocket.hpp"
	"include/iserver.hpp"
	"include/itask.hpp"
	"include/waitable.hpp"
	"include/permissions.hpp"
	"include/threadpool.hpp"
)

set(PROJECT_PRIVATE "")
list(APPEND PROJECT_PRIVATE
	"source/threadpool.cpp"
)

set(PROJECT_TEMPLATES "")
list(APPEND PROJECT_TEMPLATES
	"${PROJECT_SOURCE_DIR}/cmake/version.hpp.in"
)

set(PROJECT_GENERATED "")
list(APPEND PROJECT_GENERATED
	"${PROJECT_BINARY_DIR}/generated/version.hpp"
)

set(PROJECT_DATA "")
list(APPEND PROJECT_DATA
	"README.md"
	"LICENSE"
)

set(PROJECT_LIBRARIES "")

set(PROJECT_DEFINES "")

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
source_group(TREE "${PROJECT_SOURCE_DIR}" PREFIX "Data Files" FILES ${PROJECT_DATA})
source_group(TREE "${PROJECT_SOURCE_DIR}/cmake" PREFIX "Template Files" FILES ${PROJECT_TEMPLATES})
source_group(TREE "${PROJECT_BINARY_DIR}/generated" PREFIX "Generated Files" FILES ${PROJECT_GENERATED})
source_group(TREE "${PROJECT_SOURCE_DIR}/include" PREFIX "Exported Files" FILES ${PROJECT_PUBLIC})

# Filter Sources
set(_TMP_SOURCE ${PROJECT_PRIVATE})
list(FILTER _TMP_SOURCE INCLUDE REGEX "\.(c|cpp)$")
source_group(TREE "${PROJECT_SOURCE_DIR}/source" PREFIX "Source Files" FILES ${_TMP_SOURCE})

# Filter Headers
set(_TMP_HEADER ${PROJECT_PRIVATE})
list(FILTER _TMP_HEADER INCLUDE REGEX "\.(h|hpp)$")
source_group(TREE "${PROJECT_SOURCE_DIR}/source" PREFIX "Header Files" FILES ${_TMP_HEADER})

#================================================================================#
# Building
#================================================================================#

# Library definition
set(_BUILD_TYPE)
if(${PropertyPrefix}MAKE_DYNAMIC)
	if(${PropertyPrefix}MAKE_MODULE)
		set(_BUILD_TYPE MODULE)
	else()
		set(_BUILD_TYPE SHARED)
	endif()
else()
	set(_BUILD_TYPE STATIC)
endif()

add_library(${PROJECT_NAME} ${_BUILD_TYPE}
	${PROJECT_PUBLIC}
	${PROJECT_PRIVATE}
	${PROJECT_GENERATED}
	${PROJECT_TEMPLATES}
	${PROJECT_DATA}
)

# Clang
if("${PropertyPrefix}" STREQUAL "")
	clang_format(
		TARGETS ${PROJECT_NAME}
		DEPENDENCY
		VERSION 9.0.0
	)
endif()

# Includes
target_include_directories(${PROJECT_NAME}
	PRIVATE "source"
	PUBLIC "include"
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

#================================================================================#
# Samples
#================================================================================#
if(${PropertyPrefix}BUILD_SAMPLES)
	add_subdirectory(${PROJECT_SOURCE_DIR}/samples)
endif()
