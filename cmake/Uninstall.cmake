cmake_minimum_required(VERSION 3.14)
include("${CMAKE_CURRENT_LIST_DIR}/InstallReceipt.cmake")
if(NOT DEFINED RECEIPT)
  message(FATAL_ERROR "No native installation receipt; run make install first")
endif()
# Validate the entire receipt before removing any file.
mirnext_read_receipt("${RECEIPT}" prefix paths)
foreach(path IN LISTS paths)
  file(REMOVE "${path}")
  if(EXISTS "${path}" OR IS_SYMLINK "${path}")
    message(FATAL_ERROR "Could not remove installed file: ${path}")
  endif()
endforeach()
# Keep the receipt so another uninstall is harmless. Leave all directories and
# unlisted files intact, including other packages sharing the prefix.
message(STATUS "Removed recorded native installation at ${prefix}")
