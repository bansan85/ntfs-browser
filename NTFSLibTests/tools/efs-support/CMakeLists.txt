# Builds EFS data for the tests with a cipher the library does not share code
# with: the unit tests and the fuzz data generator use it.
add_library(
  NtfsEfsTestSupport
  STATIC
  "include/ntfs-browser/test/efs/efs-test-support.h"
  "efs-test-support.cpp"
)

# src/ resolves efs/fek.h.
target_include_directories(
  NtfsEfsTestSupport
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../src"
)

target_link_libraries(NtfsEfsTestSupport PUBLIC ${PROJECT_NAME})

# The feature macros mirror the library's own: the file picks its cipher
# backend with them.
target_compile_definitions(
  NtfsEfsTestSupport
  PRIVATE
    NTFS_BROWSER_EXPORT_INTERNALS_FOR_TESTS
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_CRYPTOPP}>:NTFS_BROWSER_ENABLE_EFS_CRYPTOPP>
    $<$<BOOL:${NTFS_BROWSER_ENABLE_EFS_BCRYPT}>:NTFS_BROWSER_ENABLE_EFS_BCRYPT>
)

if(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP)
  target_link_libraries(NtfsEfsTestSupport PRIVATE cryptopp::cryptopp)
endif()

if(WIN32 AND NTFS_BROWSER_ENABLE_EFS_BCRYPT)
  target_link_libraries(NtfsEfsTestSupport PRIVATE bcrypt)
endif()
