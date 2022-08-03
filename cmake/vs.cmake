function(group_src)
  foreach(FILE IN LISTS ARGN)
    # Get the directory of the source file
    get_filename_component(PARENT_DIR "${FILE}" DIRECTORY)

    # Remove common directory prefix to make the group
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}" "" GROUP "${PARENT_DIR}")

    # Make sure we are using windows slashes
    string(REPLACE "/" "\\" GROUP "${GROUP}")

    # Group into "Source Files" and "Header Files"
    if("${FILE}" MATCHES ".*\\.cpp")
      set(GROUP "Source Files\\${GROUP}")
    elseif("${FILE}" MATCHES ".*CMakeLists\\.txt")
      set(GROUP "Make Files\\${GROUP}")
    elseif("${FILE}" MATCHES ".*\\.h")
      set(GROUP "Header Files\\${GROUP}")
    endif()

    source_group("${GROUP}" FILES "${FILE}")
  endforeach()
endfunction(group_src)
