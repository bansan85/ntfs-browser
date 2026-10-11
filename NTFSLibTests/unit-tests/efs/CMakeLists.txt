target_sources(NtfsBrowserTests PRIVATE "efs-disabled-tests.cpp")

# Same "EFS master" condition (NTFS_BROWSER_EFS_MASTER, set in the root
# CMakeLists.txt) as the library's own source-list gate: these files build only
# when the library compiled efs-context.cpp et al. in.
if(NTFS_BROWSER_EFS_MASTER)
  target_sources(
    NtfsBrowserTests
    PRIVATE
      "coverage-efs-tests.cpp"
      "efs-parser-tests.cpp"
      "efs-pfx-tests.cpp"
      "efs-stream-tests.cpp"
      "efs-test-support.cpp"
      "efs-test-support.h"
      "efs-tests-support.h"
      "nps-ntfs1-efs-tests.cpp"
  )
endif()

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
