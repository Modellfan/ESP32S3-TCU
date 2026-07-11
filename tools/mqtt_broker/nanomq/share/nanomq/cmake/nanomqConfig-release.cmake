#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "nanomq" for configuration "Release"
set_property(TARGET nanomq APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(nanomq PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/nanomq.exe"
  )

list(APPEND _cmake_import_check_targets nanomq )
list(APPEND _cmake_import_check_files_for_nanomq "${_IMPORT_PREFIX}/bin/nanomq.exe" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
