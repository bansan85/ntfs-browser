# IDiskReader test doubles, shared by the unit tests and NtfsFuzzer.
add_library(
  NtfsTestDiskReaders
  STATIC
  "counting-disk-reader.h"
  "counting-disk-reader.cpp"
  "memory-disk-reader.h"
  "memory-disk-reader.cpp"
  "sequential-disk-reader.h"
  "sequential-disk-reader.cpp"
  "partition-disk-reader.h"
  "partition-disk-reader.cpp"
  "recording-disk-reader.h"
  "recording-disk-reader.cpp"
)

target_include_directories(
  NtfsTestDiskReaders
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(NtfsTestDiskReaders PUBLIC ${PROJECT_NAME})
