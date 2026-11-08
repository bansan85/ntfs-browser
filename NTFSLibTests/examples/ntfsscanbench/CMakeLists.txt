# Three ways to list a volume, timed: the scan of $MFT (mft-list-engine.cpp),
# and the walk of the directory indexes with a volume that keeps no cache or
# one that does (tools/dir-walk). --engine=all runs the three in turn.
add_executable(
  NtfsScanBench
  WIN32
  "arguments.h"
  "arguments.cpp"
  "comparison-table.h"
  "comparison-table.cpp"
  "display-paths.h"
  "display-paths.cpp"
  "engine.h"
  "main.cpp"
  "mft-list-engine.cpp"
  "run-engine.cpp"
  "scan-bench.h"
  "scan-bench.cpp"
  "walk-engine.cpp"
)
# The listing is written by a thread of its own.
find_package(Threads REQUIRED)
target_link_libraries(
  NtfsScanBench
  PRIVATE ${PROJECT_NAME} NtfsDeviceAccess NtfsDirWalk Threads::Threads
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
