# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

# Python extension module `datapath._core`, built by scikit-build-core
# (see pyproject.toml in the repository root).

find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module
	OPTIONAL_COMPONENTS Development.SABIModule)
execute_process(
	COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
	OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT
)
find_package(nanobind CONFIG REQUIRED)

# STABLE_ABI: one abi3 wheel covers CPython 3.12+ (ignored for older and
# free-threaded interpreters). FREE_THREADED: no GIL on 3.14t+.
# LTO + NOMINSIZE: optimize for speed, not size.
nanobind_add_module(_core STABLE_ABI FREE_THREADED LTO NOMINSIZE
	src/module.cpp
)
target_link_libraries(_core PRIVATE datapath::datapath)
target_include_directories(_core PRIVATE "${PROJECT_SOURCE_DIR}/source")

# Lay out an importable package in the build tree (used for stubs and tests).
set(DATAPATH_PY_STAGE "${CMAKE_CURRENT_BINARY_DIR}/package")
set_target_properties(_core PROPERTIES
	LIBRARY_OUTPUT_DIRECTORY "${DATAPATH_PY_STAGE}/datapath"
	LIBRARY_OUTPUT_DIRECTORY_RELEASE "${DATAPATH_PY_STAGE}/datapath"
	LIBRARY_OUTPUT_DIRECTORY_DEBUG "${DATAPATH_PY_STAGE}/datapath"
	LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${DATAPATH_PY_STAGE}/datapath"
)
file(GLOB DATAPATH_PY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/datapath/*.py")
foreach(_file ${DATAPATH_PY_SOURCES})
	get_filename_component(_name "${_file}" NAME)
	configure_file("${_file}" "${DATAPATH_PY_STAGE}/datapath/${_name}" COPYONLY)
endforeach()

install(TARGETS _core LIBRARY DESTINATION datapath COMPONENT python)

# Type stubs for the native module, generated from its docstrings and signatures.
# Generating them imports the module. A macOS build for the other architecture
# (x86_64 wheels built on arm64) is not flagged as cross-compiling, so run
# Python as that architecture (Rosetta): nanobind_add_stub() runs
# Python_EXECUTABLE, so point it at a wrapper script for this call only.
function(datapath_add_stub)
	if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "^(x86_64|arm64)$"
		AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
		set(_wrapper "${CMAKE_CURRENT_BINARY_DIR}/python-${CMAKE_OSX_ARCHITECTURES}.sh")
		file(WRITE "${_wrapper}" "#!/bin/sh\nexec arch -${CMAKE_OSX_ARCHITECTURES} \"${Python_EXECUTABLE}\" \"$@\"\n")
		execute_process(COMMAND chmod +x "${_wrapper}")
		set(Python_EXECUTABLE "${_wrapper}")
	endif()
	nanobind_add_stub(${ARGN})
endfunction()
if(NOT CMAKE_CROSSCOMPILING)
	datapath_add_stub(_core_stub
		MODULE datapath._core
		OUTPUT "${DATAPATH_PY_STAGE}/datapath/_core.pyi"
		PYTHON_PATH "${DATAPATH_PY_STAGE}"
		DEPENDS _core
		MARKER_FILE "${DATAPATH_PY_STAGE}/datapath/py.typed"
	)
	install(FILES "${DATAPATH_PY_STAGE}/datapath/_core.pyi" "${DATAPATH_PY_STAGE}/datapath/py.typed" DESTINATION datapath COMPONENT python)
endif()
