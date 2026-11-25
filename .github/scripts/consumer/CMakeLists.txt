# Low Latency IPC Library for high-speed traffic
# Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
#
# SPDX-License-Identifier: BSD-3-Clause

# Builds a C and a C++ program against an *installed* DataPath, the way package
# users would. Used by release.yml to check the prebuilt archives:
#   cmake -S .github/scripts/consumer -B build -DCMAKE_PREFIX_PATH=<install prefix>
cmake_minimum_required(VERSION 3.21)
project(datapath_consumer LANGUAGES C CXX)

find_package(datapath 1 CONFIG REQUIRED)
message(STATUS "Found datapath ${datapath_VERSION} (shared: ${DATAPATH_SHARED_LIBRARY})")

enable_testing()
foreach(lang c cpp)
	add_executable(consumer_${lang} main.${lang})
	target_link_libraries(consumer_${lang} PRIVATE datapath::datapath)
	if(WIN32)
		# Put the DataPath DLL next to the program so it runs from the build tree.
		add_custom_command(TARGET consumer_${lang} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				$<TARGET_RUNTIME_DLLS:consumer_${lang}> $<TARGET_FILE_DIR:consumer_${lang}>
			COMMAND_EXPAND_LISTS)
	endif()
	add_test(NAME consumer_${lang} COMMAND consumer_${lang})
endforeach()
if(APPLE OR UNIX)
	set_tests_properties(consumer_c consumer_cpp PROPERTIES
		ENVIRONMENT "LD_LIBRARY_PATH=${datapath_DIR}/../..;DYLD_LIBRARY_PATH=${datapath_DIR}/../..")
endif()
