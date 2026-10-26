# Embedded in the console tools on Windows, see the file for what it does.
set(
  NTFS_BROWSER_LONG_PATH_MANIFEST
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/long-path-aware.manifest"
)

add_subdirectory("tools")
add_subdirectory("examples")
add_subdirectory("fuzz")
add_subdirectory("unit-tests")
