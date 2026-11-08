add_executable(
  NtfsScanBench
  WIN32
  "arguments.h"
  "arguments.cpp"
  "display-paths.h"
  "display-paths.cpp"
  "main.cpp"
)
# The listing is written by a thread of its own.
find_package(Threads REQUIRED)
target_link_libraries(
  NtfsScanBench
  PRIVATE ${PROJECT_NAME} NtfsDeviceAccess Threads::Threads
)
set_target_properties(NtfsScanBench PROPERTIES WIN32_EXECUTABLE 0)
if(WIN32)
  target_sources(NtfsScanBench PRIVATE "${NTFS_BROWSER_LONG_PATH_MANIFEST}")
endif()
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsScanBench
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsScanBench>
      $<TARGET_FILE_DIR:NtfsScanBench>
    COMMAND_EXPAND_LISTS
  )
endif()
