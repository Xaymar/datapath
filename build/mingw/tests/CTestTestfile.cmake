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
