# The command line and the listing logic, shared with the unit tests
# (unit-tests/io/ntfsdir-*-tests.cpp). main.cpp only hands over argv.
add_library(
  NtfsDirCore
  STATIC
  "include/ntfs-browser/example/ntfsdir/arguments.h"
  "arguments.cpp"
  "listing-printer.h"
  "listing-printer.cpp"
  "include/ntfs-browser/example/ntfsdir/ntfsdir-core.h"
  "ntfsdir-core.cpp"
  "utf16-convert.h"
  "utf16-convert.cpp"
)
target_include_directories(
  NtfsDirCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(NtfsDirCore PUBLIC NtfsBrowserInternals NtfsDeviceAccess)

add_executable(NtfsDir WIN32 "main.cpp")
target_link_libraries(NtfsDir PRIVATE NtfsBrowserInternals NtfsDirCore)
if(WIN32)
  target_sources(NtfsDir PRIVATE "${NTFS_BROWSER_LONG_PATH_MANIFEST}")
endif()
set_target_properties(NtfsDir PROPERTIES WIN32_EXECUTABLE 0)
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsDir
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsDir>
      $<TARGET_FILE_DIR:NtfsDir>
    COMMAND_EXPAND_LISTS
  )
endif()
