# NtfsBench: the measuring tool of the benchmark (replay of a read log, the
# disk's sheet, a simulated disk, the system helpers). The Python script
# NtfsBenchmark.py drives it; see the README.
add_executable(
  NtfsBench
  WIN32
  "bench-tool.h"
  "diskbench-command.cpp"
  "main.cpp"
  "replay-command.cpp"
  "simulate-command.cpp"
  "system-commands.cpp"
)
find_package(Threads REQUIRED)
target_link_libraries(
  NtfsBench
  PRIVATE
    ${PROJECT_NAME}
    NtfsDeviceAccess
    NtfsFakeImage
    NtfsTestDiskReaders
    Threads::Threads
)
set_target_properties(NtfsBench PROPERTIES WIN32_EXECUTABLE 0)
if(WIN32)
  target_sources(NtfsBench PRIVATE "${NTFS_BROWSER_LONG_PATH_MANIFEST}")
endif()
if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsBench
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsBench>
      $<TARGET_FILE_DIR:NtfsBench>
    COMMAND_EXPAND_LISTS
  )
endif()
