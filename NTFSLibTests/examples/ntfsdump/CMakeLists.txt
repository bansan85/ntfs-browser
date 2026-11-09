# The dump logic, shared with the unit tests
# (unit-tests/io/ntfsdump-core-tests.cpp). The command line, the volume opening
# and the run all live here. main.cpp only hands over argv.
add_library(
  NtfsDumpCore
  STATIC
  "include/ntfs-browser/example/ntfsdump/ntfsdump-core.h"
  "ntfsdump-core.cpp"
)
target_include_directories(
  NtfsDumpCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(NtfsDumpCore PUBLIC NtfsBrowserInternals NtfsDeviceAccess)

add_executable(NtfsDump WIN32 "main.cpp")
target_link_libraries(NtfsDump PRIVATE NtfsBrowserInternals NtfsDumpCore)
if(WIN32)
  target_sources(NtfsDump PRIVATE "${NTFS_BROWSER_LONG_PATH_MANIFEST}")
endif()
set_target_properties(NtfsDump PROPERTIES WIN32_EXECUTABLE 0)
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsDump
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsDump>
      $<TARGET_FILE_DIR:NtfsDump>
    COMMAND_EXPAND_LISTS
  )
endif()
