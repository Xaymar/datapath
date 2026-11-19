# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

# Node-API addon, built by cmake-js (see package.json). Links the C++ library
# statically, so the .node file has no other runtime dependency.

cmake_minimum_required(VERSION 3.16)
project(datapath_node LANGUAGES CXX)

if(NOT CMAKE_JS_VERSION)
	message(FATAL_ERROR "Build this directory through cmake-js: npm run build")
endif()

# The npm tarball carries a copy of the library in deps/datapath (scripts/vendor.js);
# a git checkout uses the repository root.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/datapath/CMakeLists.txt")
	set(DATAPATH_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/datapath")
else()
	get_filename_component(DATAPATH_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(DATAPATH_SHARED OFF CACHE BOOL "" FORCE)
set(DATAPATH_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DATAPATH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${DATAPATH_SOURCE_DIR}" datapath EXCLUDE_FROM_ALL)

execute_process(
	COMMAND node -p "require('node-addon-api').include_dir"
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	OUTPUT_VARIABLE NODE_ADDON_API_DIR
	OUTPUT_STRIP_TRAILING_WHITESPACE
)
get_filename_component(NODE_ADDON_API_DIR "${NODE_ADDON_API_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(datapath_node SHARED src/addon.cpp ${CMAKE_JS_SRC})
set_target_properties(datapath_node PROPERTIES
	PREFIX ""
	SUFFIX ".node"
	OUTPUT_NAME "datapath"
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED ON
	CXX_VISIBILITY_PRESET hidden
)
target_include_directories(datapath_node PRIVATE ${CMAKE_JS_INC} "${NODE_ADDON_API_DIR}")
target_compile_definitions(datapath_node PRIVATE NAPI_VERSION=8 NAPI_CPP_EXCEPTIONS NODE_ADDON_API_DISABLE_DEPRECATED)
target_link_libraries(datapath_node PRIVATE datapath::datapath ${CMAKE_JS_LIB})

if(MSVC)
	target_compile_options(datapath_node PRIVATE /W4 /EHsc /utf-8)
else()
	target_compile_options(datapath_node PRIVATE -Wall -Wextra)
endif()

if(MINGW)
	target_link_options(datapath_node PRIVATE -static -static-libgcc -static-libstdc++)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
	# Portable prebuilds: no dependency on the build machine's libstdc++ version.
	option(DATAPATH_NODE_STATIC_LIBSTDCXX "Link libstdc++ statically into the addon." ON)
	if(DATAPATH_NODE_STATIC_LIBSTDCXX)
		target_link_options(datapath_node PRIVATE -static-libstdc++ -static-libgcc)
	endif()
endif()

if(MSVC AND CMAKE_JS_NODELIB_DEF AND CMAKE_JS_NODELIB_TARGET)
	# Generate node.lib from node-api-headers' .def file.
	execute_process(COMMAND ${CMAKE_AR} /def:${CMAKE_JS_NODELIB_DEF} /out:${CMAKE_JS_NODELIB_TARGET}
		${CMAKE_STATIC_LINKER_FLAGS})
endif()
