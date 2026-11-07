target_sources(NtfsBrowserTests PRIVATE "efs-disabled-tests.cpp")

# Same "EFS master" condition (NTFS_BROWSER_EFS_MASTER, set in the root
# CMakeLists.txt) as the library's own source-list gate: these files build only
# when the library compiled efs-context.cpp et al. in.
if(NTFS_BROWSER_EFS_MASTER)
  target_sources(
    NtfsBrowserTests
    PRIVATE
      "coverage-efs-tests.cpp"
      "efs-context-tests.cpp"
      "efs-parser-tests.cpp"
      "efs-pfx-tests.cpp"
      "efs-stream-limits-tests.cpp"
      "efs-stream-tests.cpp"
      "include/ntfs-browser/test/efs/efs-tests-support.h"
      "nps-ntfs1-efs-tests.cpp"
  )
  target_link_libraries(NtfsBrowserTests PRIVATE NtfsEfsTestSupport)
endif()

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
