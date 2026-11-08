# The directory walk of NtfsDir as it was before Search::Scan: the nocache and
# fullcache engines of NtfsScanBench, and the oracle of the read ahead tests
# (unit-tests/io/). Portable C++, the POSIX half sits behind #ifndef _WIN32.
add_library(
  NtfsDirWalk
  STATIC
  "include/ntfs-browser/tool/dir-walk/directory-walk.h"
  "directory-walk.cpp"
)
target_include_directories(
  NtfsDirWalk
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(NtfsDirWalk PUBLIC ${PROJECT_NAME} NtfsDeviceAccess)
