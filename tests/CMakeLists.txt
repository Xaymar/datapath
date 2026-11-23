# Tests that reach into internals link datapath_internal: the library itself in
# a static build, and a static copy of the same objects in a shared build, whose
# internals are not exported. Every other test links the library as shipped.
if(DATAPATH_SHARED)
	add_library(datapath_internal STATIC $<TARGET_OBJECTS:datapath_objects>)
	target_link_libraries(datapath_internal PUBLIC ${DATAPATH_PLATFORM_LIBS})
	target_include_directories(datapath_internal PUBLIC
		"${PROJECT_SOURCE_DIR}/include"
		"${PROJECT_BINARY_DIR}/generated/include"
	)
	# Use the declarations without dllimport.
	target_compile_definitions(datapath_internal PUBLIC DATAPATH_STATIC)
else()
	add_library(datapath_internal ALIAS datapath)
endif()

# Each test_*.cpp becomes one executable; each TEST() inside it one ctest entry.
# Pass INTERNAL to link datapath_internal instead of datapath.
function(datapath_add_test NAME)
	cmake_parse_arguments(PARSE_ARGV 1 ARG "INTERNAL" "" "")
	add_executable(${NAME} ${NAME}.cpp ${ARG_UNPARSED_ARGUMENTS})
	if(ARG_INTERNAL)
		target_link_libraries(${NAME} PRIVATE datapath_internal)
	else()
		target_link_libraries(${NAME} PRIVATE datapath)
	endif()
	target_include_directories(${NAME} PRIVATE "${PROJECT_SOURCE_DIR}/source")
	file(STRINGS ${NAME}.cpp _lines REGEX "^TEST\\(")
	foreach(_line ${_lines})
		string(REGEX REPLACE "^TEST\\(([A-Za-z0-9_]+)\\).*" "\\1" _test "${_line}")
		add_test(NAME ${NAME}.${_test} COMMAND ${NAME} ${_test})
		set_tests_properties(${NAME}.${_test} PROPERTIES TIMEOUT 120)
	endforeach()
endfunction()

# Run every TEST() of an existing test executable again with ENV set, as
# NAME.<test>.SUFFIX (for example the whole integration suite over shared memory).
function(datapath_add_test_variant NAME SUFFIX ENV)
	file(STRINGS ${NAME}.cpp _lines REGEX "^TEST\\(")
	foreach(_line ${_lines})
		string(REGEX REPLACE "^TEST\\(([A-Za-z0-9_]+)\\).*" "\\1" _test "${_line}")
		add_test(NAME ${NAME}.${_test}.${SUFFIX} COMMAND ${NAME} ${_test})
		set_tests_properties(${NAME}.${_test}.${SUFFIX} PROPERTIES TIMEOUT 120 ENVIRONMENT "${ENV}")
	endforeach()
endfunction()

datapath_add_test(test_api)
datapath_add_test(test_framing INTERNAL)
datapath_add_test(test_send_queue INTERNAL)
datapath_add_test(test_integration raw_peer.cpp raw_peer.hpp)
datapath_add_test(test_edge_cases)
set_tests_properties(test_integration.large_8mib_intact PROPERTIES TIMEOUT 300)
if(WIN32)
	datapath_add_test(test_windows_security INTERNAL)
	target_link_libraries(test_windows_security PRIVATE advapi32)
	datapath_add_test(test_windows_pairing INTERNAL raw_peer.cpp raw_peer.hpp)
	datapath_add_test(test_windows_shm INTERNAL)
endif()
if(NOT WIN32)
	datapath_add_test(test_posix)
endif()
if(WIN32 OR CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
	# The same suites with every connection on the shared-memory data path.
	datapath_add_test_variant(test_integration shm DATAPATH_SHARED_MEMORY=1)
	datapath_add_test_variant(test_edge_cases shm DATAPATH_SHARED_MEMORY=1)
	set_tests_properties(test_integration.large_8mib_intact.shm PROPERTIES TIMEOUT 300)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
	datapath_add_test(test_posix_shm INTERNAL)
endif()
datapath_add_test(test_c_api c_api_echo.c)
