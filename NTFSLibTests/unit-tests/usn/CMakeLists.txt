target_sources(
  NtfsBrowserTests
  PRIVATE
    "usn-benchmark-tests.cpp"
    "usn-cap-tests.cpp"
    "usn-failure-cache-tests.cpp"
    "usn-journal-hint-tests.cpp"
    "usn-journal-load-tests.cpp"
    "include/ntfs-browser/test/usn/usn-journal-record.h"
    "usn-mft-tree-tests.cpp"
    "usn-record-scanner-tests.cpp"
    "usn-resync-tests.cpp"
    "usn-sparse-read-tests.cpp"
    "usn-strict-mode-tests.cpp"
    "include/ntfs-browser/test/usn/usn-test-support.h"
)

target_include_directories(
  NtfsBrowserTests
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
