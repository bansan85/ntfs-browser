# Portable C++, no Win32-only headers outside their own #ifdef _WIN32 blocks,
# so it builds on Linux and Windows alike (see os-api-walk.cpp/volume-open.cpp
# for the per-platform halves).
add_executable(
  NtfsCompare
  "main.cpp"
  "entry.h"
  "time-convert.h"
  "compare-engine.h"
  "compare-engine.cpp"
  "std-filesystem-walk.h"
  "std-filesystem-walk.cpp"
  "library-walk.h"
  "library-walk.cpp"
  "os-api-walk.h"
  "os-api-walk.cpp"
  "volume-open.h"
  "volume-open.cpp"
  "raw-device-disk-reader.h"
  "raw-device-disk-reader.cpp"
  "linux-utf8.h"
  "linux-utf8.cpp"
)

target_link_libraries(NtfsCompare PRIVATE ${PROJECT_NAME})

if(WIN32 AND BUILD_SHARED_LIBS)
  add_custom_command(
    TARGET NtfsCompare
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NtfsCompare>
      $<TARGET_FILE_DIR:NtfsCompare>
    COMMAND_EXPAND_LISTS
  )
endif()
