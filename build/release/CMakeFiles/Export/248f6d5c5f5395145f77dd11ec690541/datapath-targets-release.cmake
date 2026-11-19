#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "datapath::datapath" for configuration "Release"
set_property(TARGET datapath::datapath APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(datapath::datapath PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libdatapath.a"
  )

list(APPEND _cmake_import_check_targets datapath::datapath )
list(APPEND _cmake_import_check_files_for_datapath::datapath "${_IMPORT_PREFIX}/lib/libdatapath.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
