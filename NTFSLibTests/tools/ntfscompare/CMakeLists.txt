# Portable C++, no Win32-only headers outside their own #ifdef _WIN32 blocks,
# so it builds on Linux and Windows alike (see os-api-walk.cpp/volume-open.cpp
# for the per-platform halves).
#
# The walkers and the comparison, everything but main(), sit in a static
# library so the unit tests can link them (unit-tests/ntfscompare/).
add_library(
  NtfsCompareCore
  STATIC
  "include/ntfs-browser/tool/ntfscompare/compare-engine.h"
  "compare-engine.cpp"
  "console.h"
  "include/ntfs-browser/tool/ntfscompare/entry.h"
  "time-convert.h"
  "include/ntfs-browser/tool/ntfscompare/std-filesystem-walk.h"
  "std-filesystem-walk.cpp"
  "include/ntfs-browser/tool/ntfscompare/library-walk.h"
  "library-walk.cpp"
  "include/ntfs-browser/tool/ntfscompare/os-api-walk.h"
  "os-api-walk.cpp"
  "include/ntfs-browser/tool/ntfscompare/volume-open.h"
  "volume-open.cpp"
)
target_include_directories(
  NtfsCompareCore
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(NtfsCompareCore PUBLIC ${PROJECT_NAME} NtfsDeviceAccess)

add_executable(NtfsCompare "main.cpp")
target_link_libraries(NtfsCompare PRIVATE ${PROJECT_NAME} NtfsCompareCore)
if(WIN32)
  target_sources(NtfsCompare PRIVATE "${NTFS_BROWSER_LONG_PATH_MANIFEST}")
endif()

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
