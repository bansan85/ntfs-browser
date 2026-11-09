# IDiskReader test doubles, shared by the unit tests and NtfsFuzzer.
add_library(
  NtfsTestDiskReaders
  STATIC
  "include/ntfs-browser/test/disk-readers/counting-disk-reader.h"
  "counting-disk-reader.cpp"
  "include/ntfs-browser/test/disk-readers/memory-disk-reader.h"
  "memory-disk-reader.cpp"
  "include/ntfs-browser/test/disk-readers/sequential-disk-reader.h"
  "sequential-disk-reader.cpp"
  "include/ntfs-browser/test/disk-readers/partition-disk-reader.h"
  "partition-disk-reader.cpp"
  "include/ntfs-browser/test/disk-readers/recording-disk-reader.h"
  "recording-disk-reader.cpp"
  "include/ntfs-browser/test/disk-readers/standalone-mft-disk-reader.h"
  "standalone-mft-disk-reader.cpp"
  "include/ntfs-browser/test/disk-readers/throttled-disk-reader.h"
  "throttled-disk-reader.cpp"
)

target_include_directories(
  NtfsTestDiskReaders
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(NtfsTestDiskReaders PUBLIC NtfsBrowserInternals)
