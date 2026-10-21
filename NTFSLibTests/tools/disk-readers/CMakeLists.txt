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
  "standalone-mft-disk-reader.h"
  "standalone-mft-disk-reader.cpp"
)

# src/ resolves the on-disk layouts standalone-mft-disk-reader.cpp reads and
# builds (boot sector, file record header, non-resident attribute header).
target_include_directories(
  NtfsTestDiskReaders
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../src"
)

target_link_libraries(NtfsTestDiskReaders PUBLIC ${PROJECT_NAME})
