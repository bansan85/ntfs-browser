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
    NtfsBrowserInternals
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

# The tests of ntfs_benchmark.py (unittest, standard library only). The script
# needs Python 3.12, so without it the tests are not registered. -I keeps the
# environment and the current folder out of the interpreter's search path.
find_package(Python3 3.12 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(
    NAME NtfsBenchmarkScript
    COMMAND
      Python3::Interpreter -I
      "${CMAKE_CURRENT_SOURCE_DIR}/test_ntfs_benchmark.py" --bench-exe
      $<TARGET_FILE:NtfsBench>
  )
  set_tests_properties(NtfsBenchmarkScript PROPERTIES TIMEOUT 600)
else()
  message(
    STATUS
    "Python 3.12 or later not found: the tests of ntfs_benchmark.py are not registered"
  )
endif()
