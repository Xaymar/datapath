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
set(VERSION_COMMIT "")
set(VERSION_SUFFIX "")
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
		set(VERSION_COMMIT ${GIT_OUTPUT})
	endif()
endif()

# Define Project
project(
	DataPath
	LANGUAGES CXX
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
option(${PropertyPrefix}BUILD_SHARED "Create dynamically linked library instead of statically linked library." ON)
option(${PropertyPrefix}BUILD_SAMPLES "Build Samples" OFF)

#================================================================================#
# Sources
#================================================================================#
set(PROJECT_PUBLIC "")
set(PROJECT_PUBLIC_INCLUDES "")
set(PROJECT_PRIVATE "")
set(PROJECT_PRIVATE_INCLUDES "")
set(PROJECT_TEMPLATES "")
set(PROJECT_GENERATED "")
set(PROJECT_DATA "")
set(PROJECT_LIBRARIES "")
set(PROJECT_DEFINES "")

# Generic Source files
list(APPEND PROJECT_PUBLIC
	"include/datapath/datapath.hpp"
	"include/datapath/error.hpp"
	"include/datapath/event.hpp"
	"include/datapath/server.hpp"
	"include/datapath/socket.hpp"
)
list(APPEND PROJECT_PUBLIC_INCLUDES
	"${PROJECT_SOURCE_DIR}/include"
	"${PROJECT_BINARY_DIR}/generated"
)
list(APPEND PROJECT_PRIVATE_INCLUDES
	"${PROJECT_SOURCE_DIR}/include/datapath"
	"${PROJECT_BINARY_DIR}/generated/datapath"
	"${PROJECT_SOURCE_DIR}/source"
)
list(APPEND PROJECT_TEMPLATES
	"cmake/config.hpp.in"
)
list(APPEND PROJECT_GENERATED
	"${PROJECT_BINARY_DIR}/generated/datapath/config.hpp"
)
list(APPEND PROJECT_DATA
	"README.md"
	"LICENSE"
)

# Create config file.
set(GEN_VERSION_MAJOR ${VERSION_MAJOR})
set(GEN_VERSION_MINOR ${VERSION_MINOR})
set(GEN_VERSION_PATCH ${VERSION_PATCH})
set(GEN_VERSION_TWEAK ${VERSION_TWEAK})
set(GEN_VERSION_SUFFIX ${VERSION_SUFFIX})
set(GEN_VERSION_COMMIT ${VERSION_COMMIT})
if(BUILD_SHARED)
	set(DATAPATH_SHARED_LIBRARY TRUE)
else()
	set(DATAPATH_SHARED_LIBRARY FALSE)
endif()
configure_file(
	"${PROJECT_SOURCE_DIR}/cmake/config.hpp.in"
	"${PROJECT_BINARY_DIR}/generated/datapath/config.hpp"
	@ONLY
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
		"source/windows/windows-overlapped.hpp"
		"source/windows/windows-overlapped.cpp"
		"source/windows/windows-socket.hpp"
		"source/windows/windows-socket.cpp"
		"source/windows/windows-client-socket.hpp"
		"source/windows/windows-client-socket.cpp"
		"source/windows/windows-server.hpp"
		"source/windows/windows-server.cpp"
		"source/windows/windows-server-socket.hpp"
		"source/windows/windows-server-socket.cpp"
		"source/windows/windows-utility.hpp"
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
source_group(TREE "${PROJECT_SOURCE_DIR}/include" PREFIX "Public Files" FILES ${PROJECT_PUBLIC})
source_group(TREE "${PROJECT_SOURCE_DIR}/source" PREFIX "Private Files" FILES ${PROJECT_PRIVATE})

#================================================================================#
# Building
#================================================================================#

# Define the target
set(_BUILD_TYPE "STATIC")
if(${PropertyPrefix}BUILD_SHARED)
	set(_BUILD_TYPE "SHARED")
	if(MSVC)
		list(APPEND PROJECT_DEFINES
			"DATAPATH_INTERFACE=__declspec(dllexport)"
		)
	else()
		list(APPEND PROJECT_DEFINES
			"DATAPATH_INTERFACE=__attribute__((dllexport))"
		)
	endif()
else()

endif()

add_library(${PROJECT_NAME} ${_BUILD_TYPE}
	${PROJECT_PUBLIC}
	${PROJECT_PRIVATE}
	${PROJECT_GENERATED}
	${PROJECT_TEMPLATES}
	${PROJECT_DATA}
)
target_include_directories(${PROJECT_NAME}
	PUBLIC
		${PROJECT_PUBLIC_INCLUDES}
	PRIVATE
		${PROJECT_PRIVATE_INCLUDES}
)
target_compile_definitions(${PROJECT_NAME}
	PRIVATE ${PROJECT_DEFINES}
)
target_link_libraries(${PROJECT_NAME}
	${PROJECT_LIBRARIES}
)
set_target_properties(
	${PROJECT_NAME}
	PROPERTIES
		CXX_STANDARD 17
		CXX_EXTENSIONS OFF
)

# File Version
if(WIN32)
	set_target_properties(
		${PROJECT_NAME}
		PROPERTIES
		VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${PROJECT_VERSION_TWEAK}
		SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${PROJECT_VERSION_TWEAK}
	)
elseif(UNIX AND NOT APPLE)
	set_target_properties(
		${PROJECT_NAME}
		PROPERTIES
		VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${PROJECT_VERSION_TWEAK}
		SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${PROJECT_VERSION_TWEAK}
	)
endif()

# Clang
if("${PropertyPrefix}" STREQUAL "")
	clang_format(
		TARGETS ${PROJECT_NAME}
		DEPENDENCY
		VERSION 9.0.0
	)
endif()

#================================================================================#
# Samples
#================================================================================#
if(${PropertyPrefix}BUILD_TESTS)
	add_subdirectory(tests)
endif()

if(${PropertyPrefix}BUILD_SAMPLES)
	add_subdirectory(samples)
endif()
