# CMake generated Testfile for 
# Source directory: /home/user/datapath/tests
# Build directory: /home/user/datapath/build/mingw/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(test_api.error_to_string "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_api.exe" "error_to_string")
set_tests_properties(test_api.error_to_string PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;14;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_api.options_defaults "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_api.exe" "options_defaults")
set_tests_properties(test_api.options_defaults PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;14;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_api.default_context_is_shared "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_api.exe" "default_context_is_shared")
set_tests_properties(test_api.default_context_is_shared PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;14;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_framing.parser_every_split_offset "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_framing.exe" "parser_every_split_offset")
set_tests_properties(test_framing.parser_every_split_offset PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;15;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_framing.parser_many_small_per_read "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_framing.exe" "parser_many_small_per_read")
set_tests_properties(test_framing.parser_many_small_per_read PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;15;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_framing.parser_large_frame_beyond_buffer "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_framing.exe" "parser_large_frame_beyond_buffer")
set_tests_properties(test_framing.parser_large_frame_beyond_buffer PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;15;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_framing.parser_rejects_oversize_header "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_framing.exe" "parser_rejects_oversize_header")
set_tests_properties(test_framing.parser_rejects_oversize_header PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;15;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_framing.hello_roundtrip "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_framing.exe" "hello_roundtrip")
set_tests_properties(test_framing.hello_roundtrip PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;15;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_framing.hello_bad_magic "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_framing.exe" "hello_bad_magic")
set_tests_properties(test_framing.hello_bad_magic PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;15;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_framing.hello_bad_version "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_framing.exe" "hello_bad_version")
set_tests_properties(test_framing.hello_bad_version PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;15;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_framing.spin_zero_budget_blocks_immediately "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_framing.exe" "spin_zero_budget_blocks_immediately")
set_tests_properties(test_framing.spin_zero_budget_blocks_immediately PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;15;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
add_test(test_framing.spin_work_defers_block "/home/user/datapath/cmake/toolchains/wine-run.sh" "/home/user/datapath/build/mingw/tests/test_framing.exe" "spin_work_defers_block")
set_tests_properties(test_framing.spin_work_defers_block PROPERTIES  TIMEOUT "120" _BACKTRACE_TRIPLES "/home/user/datapath/tests/CMakeLists.txt;9;add_test;/home/user/datapath/tests/CMakeLists.txt;15;datapath_add_test;/home/user/datapath/tests/CMakeLists.txt;0;")
